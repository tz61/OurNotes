#include "formats/bibtex.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "core/schema.h"
#include "core/util.h"
#include "formats/common.h"

namespace on::formats {

namespace {

using schema::getField;

bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool allDigits(std::string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// =================================================================================
// LaTeX -> Unicode
// =================================================================================

struct Accent {
    char cmd;
    uint32_t combining;  // used when no precomposed character is listed
    const char* table;   // space separated "<base><composed>" pairs
};

const Accent kAccents[] = {
    {'"', 0x308, "aä eë iï oö uü yÿ AÄ EË IÏ OÖ UÜ YŸ"},
    {'\'', 0x301, "aá eé ií oó uú yý AÁ EÉ IÍ OÓ UÚ YÝ cć CĆ nń NŃ sś SŚ zź ZŹ lĺ LĹ rŕ RŔ gǵ GǴ"},
    {'`', 0x300, "aà eè iì oò uù AÀ EÈ IÌ OÒ UÙ nǹ NǸ"},
    {'^', 0x302, "aâ eê iî oô uû AÂ EÊ IÎ OÔ UÛ cĉ CĈ gĝ GĜ hĥ HĤ jĵ JĴ sŝ SŜ wŵ WŴ yŷ YŶ"},
    {'~', 0x303, "aã nñ oõ AÃ NÑ OÕ iĩ IĨ uũ UŨ"},
    {'=', 0x304, "aā eē iī oō uū AĀ EĒ IĪ OŌ UŪ"},
    {'.', 0x307, "zż ZŻ eė EĖ gġ GĠ cċ CĊ Iİ"},
    {'u', 0x306, "aă AĂ gğ GĞ uŭ UŬ eĕ EĔ iĭ IĬ oŏ OŎ"},
    {'v', 0x30C, "cč CČ sš SŠ zž ZŽ rř RŘ eě EĚ nň NŇ dď DĎ tť TŤ lľ LĽ aǎ AǍ iǐ IǏ oǒ OǑ uǔ UǓ gǧ GǦ kǩ KǨ"},
    {'H', 0x30B, "oő OŐ uű UŰ"},
    {'c', 0x327, "cç CÇ sş SŞ tţ TŢ gģ GĢ kķ KĶ lļ LĻ nņ NŅ rŗ RŖ eȩ EȨ"},
    {'k', 0x328, "aą AĄ eę EĘ iį IĮ uų UŲ"},
    {'r', 0x30A, "aå AÅ uů UŮ"},
    {'d', 0x323, ""},
    {'b', 0x331, ""},
    {'t', 0x361, ""},
};

const Accent* accentFor(char c)
{
    for (auto& a : kAccents)
        if (a.cmd == c)
            return &a;
    return nullptr;
}

std::string compose(const Accent& a, const std::string& base)
{
    if (base.empty())
        return {};
    if (base.size() == 1) {
        std::string_view table = a.table;
        size_t pos = 0;
        while (pos < table.size()) {
            size_t end = table.find(' ', pos);
            if (end == std::string_view::npos)
                end = table.size();
            if (table[pos] == base[0])
                return std::string(table.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        }
    }
    std::string out = base;
    appendUtf8(out, a.combining);
    return out;
}

const std::map<std::string, const char*>& symbols()
{
    static const std::map<std::string, const char*> m = {
        // letters
        {"ss", "ß"}, {"o", "ø"}, {"O", "Ø"}, {"aa", "å"}, {"AA", "Å"}, {"ae", "æ"}, {"AE", "Æ"}, {"oe", "œ"},
        {"OE", "Œ"}, {"l", "ł"}, {"L", "Ł"}, {"i", "ı"}, {"j", "ȷ"}, {"dh", "ð"}, {"DH", "Ð"}, {"th", "þ"},
        {"TH", "Þ"}, {"ng", "ŋ"}, {"NG", "Ŋ"}, {"dj", "đ"}, {"DJ", "Đ"},
        // punctuation and symbols
        {"textendash", "–"}, {"textemdash", "—"}, {"ldots", "…"}, {"dots", "…"}, {"textellipsis", "…"},
        {"textquoteleft", "‘"}, {"textquoteright", "’"}, {"textquotedblleft", "“"}, {"textquotedblright", "”"},
        {"guillemotleft", "«"}, {"guillemotright", "»"}, {"textregistered", "®"}, {"texttrademark", "™"},
        {"copyright", "©"}, {"textcopyright", "©"}, {"S", "§"}, {"P", "¶"}, {"pounds", "£"},
        {"textsterling", "£"}, {"euro", "€"}, {"texteuro", "€"}, {"textdegree", "°"}, {"degree", "°"},
        {"textbackslash", "\\"}, {"textasciitilde", "~"}, {"textunderscore", "_"}, {"textbar", "|"},
        {"textless", "<"}, {"textgreater", ">"}, {"textasciicircum", "^"}, {"textbullet", "•"},
        {"textperiodcentered", "·"}, {"dag", "†"}, {"ddag", "‡"}, {"textdagger", "†"}, {"textdaggerdbl", "‡"},
        {"textexclamdown", "¡"}, {"textquestiondown", "¿"}, {"textmu", "µ"}, {"textpm", "±"},
        {"LaTeX", "LaTeX"}, {"TeX", "TeX"}, {"BibTeX", "BibTeX"}, {"LaTeXe", "LaTeX2e"},
        // math
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ε"},
        {"zeta", "ζ"}, {"eta", "η"}, {"theta", "θ"}, {"vartheta", "ϑ"}, {"iota", "ι"}, {"kappa", "κ"},
        {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"}, {"pi", "π"}, {"varpi", "ϖ"}, {"rho", "ρ"},
        {"varrho", "ϱ"}, {"sigma", "σ"}, {"varsigma", "ς"}, {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "φ"},
        {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"}, {"Gamma", "Γ"}, {"Delta", "Δ"},
        {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Upsilon", "Υ"},
        {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"}, {"pm", "±"}, {"mp", "∓"}, {"times", "×"}, {"cdot", "·"},
        {"div", "÷"}, {"leq", "≤"}, {"le", "≤"}, {"geq", "≥"}, {"ge", "≥"}, {"neq", "≠"}, {"ne", "≠"},
        {"approx", "≈"}, {"sim", "∼"}, {"equiv", "≡"}, {"propto", "∝"}, {"infty", "∞"}, {"to", "→"},
        {"rightarrow", "→"}, {"leftarrow", "←"}, {"Rightarrow", "⇒"}, {"leftrightarrow", "↔"},
        {"partial", "∂"}, {"nabla", "∇"}, {"sum", "∑"}, {"prod", "∏"}, {"int", "∫"}, {"sqrt", "√"},
        {"in", "∈"}, {"ell", "ℓ"}, {"hbar", "ℏ"}, {"circ", "∘"}, {"star", "⋆"}, {"ast", "∗"},
        {"langle", "⟨"}, {"rangle", "⟩"}, {"forall", "∀"}, {"exists", "∃"}, {"emptyset", "∅"},
    };
    return m;
}

// Symbols that are letters glue to the following word ("Stra\ss e"), like TeX does.
bool isLetterSymbol(const std::string& name)
{
    static const std::set<std::string> letters = {"ss", "o", "O", "aa", "AA", "ae", "AE", "oe", "OE", "l",
                                                  "L",  "i", "j", "dh", "DH", "th", "TH", "ng", "NG", "dj", "DJ"};
    return letters.count(name) > 0;
}

const std::set<std::string> kKeepArgument = {
    "textit", "emph", "textbf", "textsc", "texttt", "textrm", "textsf", "textup", "textsl", "textmd",
    "textnormal", "mathrm", "mathbf", "mathit", "mathsf", "mathtt", "mathcal", "mathbb", "mathfrak",
    "boldsymbol", "mbox", "hbox", "text", "NoCaseChange", "ensuremath", "textsuperscript", "textsubscript",
    "underline", "uline", "MakeUppercase", "MakeLowercase", "uppercase", "lowercase", "enquote", "textquote"};
const std::set<std::string> kDropArgument = {"noopsort", "sortkey", "hyphenation", "nocite", "label"};

class LatexDecoder {
public:
    explicit LatexDecoder(std::string_view s, int depth = 0) : s_(s), depth_(depth) {}

    std::string decode()
    {
        std::string out;
        if (depth_ > 32) {  // pathological nesting: keep the text, drop the markup
            for (size_t i = 0; i < s_.size(); ++i) {
                if (s_[i] == '\\') {
                    while (i + 1 < s_.size() && isAlpha(s_[i + 1]))
                        ++i;
                } else if (s_[i] != '{' && s_[i] != '}') {
                    out += s_[i];
                }
            }
            return out;
        }
        while (i_ < s_.size())
            step(out);
        return out;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    int depth_;

    std::string nested(std::string_view s) const { return LatexDecoder(s, depth_ + 1).decode(); }

    bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }

    void skipSpaces()
    {
        while (i_ < s_.size() && isSpace(s_[i_]))
            ++i_;
    }

    // Raw content of the {...} group at i_; i_ moves past the closing brace.
    std::string_view group()
    {
        size_t start = ++i_;
        int depth = 1;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '\\') {
                i_ += 2;
                continue;
            }
            if (c == '{')
                ++depth;
            else if (c == '}' && --depth == 0)
                break;
            ++i_;
        }
        size_t end = std::min(i_, s_.size());
        if (i_ < s_.size())
            ++i_;
        return s_.substr(start, end - start);
    }

    void step(std::string& out)
    {
        char c = s_[i_];
        switch (c) {
        case '\\':
            command(out);
            return;
        case '{':
        case '}':
        case '$':
            ++i_;
            return;
        case '~':
            out += ' ';
            ++i_;
            return;
        case '-':
            if (s_.substr(i_, 3) == "---") {
                out += "—";
                i_ += 3;
                return;
            }
            if (s_.substr(i_, 2) == "--") {
                out += "–";
                i_ += 2;
                return;
            }
            break;
        case '`':
            if (s_.substr(i_, 2) == "``") {
                out += "“";
                i_ += 2;
                return;
            }
            break;
        case '\'':
            if (s_.substr(i_, 2) == "''") {
                out += "”";
                i_ += 2;
                return;
            }
            break;
        default:
            break;
        }
        out += c;
        ++i_;
    }

    void command(std::string& out)
    {
        ++i_;  // backslash
        if (i_ >= s_.size())
            return;
        char c = s_[i_];
        if (!isAlpha(c)) {
            if (auto* a = accentFor(c)) {
                ++i_;
                accent(out, *a);
                return;
            }
            ++i_;
            switch (c) {
            case '&': case '%': case '$': case '#': case '_': case '{': case '}':
                out += c;
                return;
            case '\\': case ' ': case ',': case ';': case ':': case '\n': case '\t':
                out += ' ';
                return;
            case '!': case '-': case '/': case '@':
                return;
            default:
                out += c;
                return;
            }
        }

        size_t start = i_;
        while (i_ < s_.size() && isAlpha(s_[i_]))
            ++i_;
        std::string name(s_.substr(start, i_ - start));

        if (name.size() == 1) {
            if (auto* a = accentFor(name[0])) {
                accent(out, *a);
                return;
            }
        }
        auto sym = symbols().find(name);
        if (sym != symbols().end()) {
            out += sym->second;
            if (isLetterSymbol(name))
                skipSpaces();
            return;
        }
        if (kKeepArgument.count(name)) {
            skipSpaces();
            if (at('{')) {
                auto inner = nested(group());
                out += (name == "enquote" || name == "textquote") ? "“" + inner + "”" : inner;
            }
            return;
        }
        if (name == "url") {
            skipSpaces();
            if (at('{'))
                out += std::string(group());
            return;
        }
        if (name == "href") {
            skipSpaces();
            if (at('{'))
                group();
            skipSpaces();
            if (at('{'))
                out += nested(group());
            return;
        }
        if (kDropArgument.count(name)) {
            skipSpaces();
            if (at('{'))
                group();
            return;
        }
        if (name == "quad" || name == "qquad" || name == "space" || name == "par" || name == "newline")
            out += ' ';
        // Unknown commands are dropped; a following {group} is kept by the main loop.
    }

    void accent(std::string& out, const Accent& a)
    {
        if (isAlpha(a.cmd))
            skipSpaces();  // \v c
        std::string base, rest;
        if (at('{')) {
            auto inner = trim(group());
            if (inner.empty()) {
                out += a.cmd;  // \~{} -> "~"
                return;
            }
            auto decoded = nested(inner);
            base = firstChar(decoded);
            rest = decoded.substr(base.size());
        } else if (at('\\')) {
            // \'\i; the argument is decoded separately so "\"\"\"..." cannot recurse deeply
            size_t start = i_++;
            if (i_ < s_.size() && isAlpha(s_[i_])) {
                while (i_ < s_.size() && isAlpha(s_[i_]))
                    ++i_;
            } else if (i_ < s_.size()) {
                ++i_;
            }
            auto decoded = nested(s_.substr(start, i_ - start));
            base = firstChar(decoded);
            rest = decoded.substr(base.size());
        } else if (i_ < s_.size()) {
            base = firstChar(s_.substr(i_));
            i_ += base.size();
        }
        if (base == "ı")
            base = "i";
        else if (base == "ȷ")
            base = "j";
        out += compose(a, base) + rest;
    }
};

std::string decodeText(std::string_view raw) { return collapseWhitespace(latexToUnicode(raw)); }

// Like decodeText but keeps blank-line paragraph breaks (abstracts, notes).
std::string decodeParagraphs(std::string_view raw)
{
    std::vector<std::string> paras;
    std::string cur;
    for (auto& line : split(raw, '\n', false)) {
        if (trim(line).empty()) {
            if (!trim(cur).empty())
                paras.push_back(decodeText(cur));
            cur.clear();
        } else {
            cur += line;
            cur += '\n';
        }
    }
    if (!trim(cur).empty())
        paras.push_back(decodeText(cur));
    return join(paras, "\n");
}

// URLs and DOIs: strip \url{} and braces, undo the usual escapes, no other decoding.
std::string rawValue(std::string_view raw)
{
    std::string s = trim(raw);
    for (std::string_view cmd : {"\\url{", "\\href{"}) {
        if (startsWith(s, cmd)) {
            auto close = s.find('}');
            s = close == std::string::npos ? s.substr(cmd.size()) : s.substr(cmd.size(), close - cmd.size());
        }
    }
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::string_view("_%#&~$").find(s[i + 1]) != std::string_view::npos) {
            out += s[++i];
        } else if (c != '{' && c != '}') {
            out += c;
        }
    }
    return trim(out);
}

// =================================================================================
// BibTeX parser
// =================================================================================

struct Entry {
    std::string type;
    std::string key;
    std::vector<std::pair<std::string, std::string>> fields;  // lower-case name -> raw value

    std::string get(std::string_view name) const
    {
        for (auto& [n, v] : fields)
            if (n == name)
                return v;
        return {};
    }
};

bool isIdentChar(char c)
{
    unsigned char u = (unsigned char)c;
    if (u <= 32 || u == 127)
        return false;
    return std::string_view("\"#%'(),={}@").find(c) == std::string_view::npos;
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text)
    {
        static const char* const abbr[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                           "jul", "aug", "sep", "oct", "nov", "dec"};
        for (int m = 0; m < 12; ++m)
            macros_[abbr[m]] = monthName(m + 1);
    }

    std::vector<Entry> parse()
    {
        std::vector<Entry> out;
        while (i_ < s_.size()) {
            size_t at = s_.find('@', i_);
            if (at == std::string_view::npos)
                break;
            i_ = at + 1;
            skipWs();
            std::string type = toLower(ident());
            skipWs();
            if (type.empty() || i_ >= s_.size() || (s_[i_] != '{' && s_[i_] != '('))
                continue;
            char close = s_[i_] == '{' ? '}' : ')';
            ++i_;
            if (type == "comment" || type == "preamble") {
                skipGroup(close);
            } else if (type == "string") {
                parseString(close);
            } else {
                Entry e;
                e.type = type;
                parseEntry(e, close);
                if (!e.fields.empty())
                    out.push_back(std::move(e));
            }
        }
        return out;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    std::map<std::string, std::string> macros_;

    bool at(char c) const { return i_ < s_.size() && s_[i_] == c; }

    void skipWs()
    {
        while (i_ < s_.size() && isSpace(s_[i_]))
            ++i_;
    }

    std::string ident()
    {
        size_t start = i_;
        while (i_ < s_.size() && isIdentChar(s_[i_]))
            ++i_;
        return std::string(s_.substr(start, i_ - start));
    }

    // Does an entry ("@type{" or "@type(") start at pos, allowing leading blanks?
    bool entryStartsAt(size_t pos) const
    {
        while (pos < s_.size() && (s_[pos] == ' ' || s_[pos] == '\t'))
            ++pos;
        if (pos >= s_.size() || s_[pos] != '@')
            return false;
        ++pos;
        size_t start = pos;
        while (pos < s_.size() && isAlpha(s_[pos]))
            ++pos;
        if (pos == start)
            return false;
        while (pos < s_.size() && isSpace(s_[pos]))
            ++pos;
        return pos < s_.size() && (s_[pos] == '{' || s_[pos] == '(');
    }

    // Skip to the next line that starts an entry (after a syntax error).
    void recover()
    {
        while (i_ < s_.size()) {
            size_t nl = s_.find('\n', i_);
            if (nl == std::string_view::npos) {
                i_ = s_.size();
                return;
            }
            i_ = nl + 1;
            if (entryStartsAt(i_))
                return;
        }
    }

    void skipGroup(char close)
    {
        char open = close == '}' ? '{' : '(';
        int depth = 0;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '\n' && entryStartsAt(i_ + 1)) {
                ++i_;
                return;
            }
            ++i_;
            if (c == open)
                ++depth;
            else if (c == close && depth-- == 0)
                return;
        }
    }

    // Reads {...} or "..." (i_ at the opening delimiter) and appends the content.
    // An unterminated value stops at a line that starts a new entry.
    bool readDelimited(char close, std::string& out)
    {
        ++i_;
        int depth = 0;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size()) {
                out += c;
                out += s_[i_ + 1];
                i_ += 2;
                continue;
            }
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                if (depth == 0) {
                    if (close == '}') {
                        ++i_;
                        return true;
                    }
                    ++i_;  // stray closing brace inside a quoted value
                    continue;
                }
                --depth;
            } else if (c == '"' && close == '"' && depth == 0) {
                ++i_;
                return true;
            } else if (c == '\n' && entryStartsAt(i_ + 1)) {
                return false;
            }
            out += c;
            ++i_;
        }
        return false;
    }

    // value := part ('#' part)*, part := {..} | ".." | number | macro
    bool readValue(std::string& out)
    {
        while (true) {
            skipWs();
            if (i_ >= s_.size())
                return false;
            char c = s_[i_];
            if (c == '{' || c == '"') {
                if (!readDelimited(c == '{' ? '}' : '"', out))
                    return false;
            } else if (std::isdigit((unsigned char)c)) {
                while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_]))
                    out += s_[i_++];
            } else {
                auto name = ident();
                if (name.empty())
                    return false;
                auto it = macros_.find(toLower(name));
                out += it != macros_.end() ? it->second : name;
            }
            skipWs();
            if (!at('#'))
                return true;
            ++i_;
        }
    }

    void parseString(char close)
    {
        skipWs();
        auto name = toLower(ident());
        skipWs();
        if (name.empty() || !at('=')) {
            recover();
            return;
        }
        ++i_;
        std::string value;
        if (!readValue(value)) {
            recover();
            return;
        }
        macros_[name] = value;
        skipWs();
        if (at(close))
            ++i_;
    }

    void parseEntry(Entry& e, char close)
    {
        skipWs();
        size_t keyStart = i_;
        while (i_ < s_.size() && s_[i_] != ',' && !isSpace(s_[i_]) && s_[i_] != close && s_[i_] != '}' &&
               s_[i_] != '=')
            ++i_;
        e.key = std::string(s_.substr(keyStart, i_ - keyStart));
        skipWs();
        if (at('=')) {  // entry without a key
            i_ = keyStart;
            e.key.clear();
        }
        while (true) {
            skipWs();
            if (i_ >= s_.size())
                return;
            char c = s_[i_];
            if (c == ',') {
                ++i_;
                continue;
            }
            if (c == close) {
                ++i_;
                return;
            }
            if (c == '@')
                return;  // missing closing delimiter; next entry starts here
            auto name = toLower(ident());
            skipWs();
            if (name.empty() || !at('=')) {
                recover();
                return;
            }
            ++i_;
            std::string value;
            if (!readValue(value)) {
                recover();
                return;
            }
            e.fields.emplace_back(name, value);
        }
    }
};

// =================================================================================
// BibTeX entry -> item
// =================================================================================

// Splits on `sep` (or any whitespace if sep == ' ') outside braces.
std::vector<std::string> splitTopLevel(std::string_view s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            cur += c;
            cur += s[++i];
            continue;
        }
        if (c == '{')
            ++depth;
        else if (c == '}')
            --depth;
        bool isSep = sep == ' ' ? isSpace(c) : c == sep;
        if (depth == 0 && isSep) {
            if (sep != ' ' || !cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (sep != ' ' || !cur.empty())
        out.push_back(cur);
    return out;
}

// Splits an author list on top-level "and".
std::vector<std::string> splitNames(std::string_view raw)
{
    std::vector<std::string> out;
    std::string cur;
    for (auto& word : splitTopLevel(raw, ' ')) {
        if (iequals(word, "and")) {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            if (!cur.empty())
                cur += ' ';
            cur += word;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

bool wholeBraced(std::string_view s)
{
    if (s.size() < 2 || s.front() != '{' || s.back() != '}' || s[1] == '\\')
        return false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == '{')
            ++depth;
        else if (s[i] == '}' && --depth == 0)
            return i == s.size() - 1;
    }
    return false;
}

// Returns null for "others".
Json parseBibName(std::string_view raw)
{
    auto s = trim(raw);
    if (s.empty() || s == "others")
        return nullptr;
    if (wholeBraced(s))
        return literalName(decodeText(std::string_view(s).substr(1, s.size() - 2)));
    auto parts = splitTopLevel(s, ',');
    if (parts.size() >= 2) {
        auto last = decodeText(parts[0]);
        auto first = decodeText(parts.back());
        if (parts.size() >= 3 && !trim(parts[1]).empty())
            first += ", " + decodeText(parts[1]);
        return personName(first, last);
    }
    auto words = splitTopLevel(s, ' ');
    if (words.size() == 1)
        return personName("", decodeText(words[0]));
    // "von" part starts at the first lower-case word that is not the last word
    size_t lastStart = words.size() - 1;
    for (size_t i = 1; i + 1 < words.size(); ++i) {
        if (words[i][0] >= 'a' && words[i][0] <= 'z') {
            lastStart = i;
            break;
        }
    }
    std::vector<std::string> first(words.begin(), words.begin() + (long)lastStart);
    std::vector<std::string> last(words.begin() + (long)lastStart, words.end());
    return personName(decodeText(join(first, " ")), decodeText(join(last, " ")));
}

// JabRef/Zotero `file` field: "Description:path:mime;..." with \: \; \\ escapes.
std::vector<std::string> parseFileField(std::string_view raw)
{
    std::string s = replaceAll(std::string(raw), "$\\backslash$", "\\");  // Mendeley
    std::vector<std::vector<std::string>> entries(1, std::vector<std::string>(1));
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == ':' || s[i + 1] == ';' || s[i + 1] == '\\')) {
            entries.back().back() += s[++i];
        } else if (c == ';') {
            entries.emplace_back(1);
        } else if (c == ':') {
            entries.back().emplace_back();
        } else if (c != '{' && c != '}') {
            entries.back().back() += c;
        }
    }
    std::vector<std::string> out;
    for (auto& parts : entries) {
        std::string path;
        if (parts.size() == 1)
            path = parts[0];
        else if (parts.size() >= 3 && parts[1].size() == 1 && isAlpha(parts[1][0]) && !parts[2].empty() &&
                 (parts[2][0] == '/' || parts[2][0] == '\\'))
            path = parts[1] + ":" + parts[2];  // unescaped Windows drive letter (JabRef)
        else
            path = parts[1];
        path = trim(path);
        if (startsWith(path, "file://"))
            path = urlDecode(path.substr(7));
        if (!path.empty() && !startsWith(path, "http://") && !startsWith(path, "https://"))
            out.push_back(path);
    }
    return out;
}

std::string mapEntryType(const std::string& t)
{
    static const std::map<std::string, std::string> m = {
        {"article", "journalArticle"},     {"inproceedings", "conferencePaper"}, {"conference", "conferencePaper"},
        {"proceedings", "book"},           {"book", "book"},                     {"mvbook", "book"},
        {"booklet", "book"},               {"collection", "book"},               {"mvcollection", "book"},
        {"manual", "book"},                {"inbook", "bookSection"},            {"incollection", "bookSection"},
        {"inreference", "encyclopediaArticle"}, {"phdthesis", "thesis"},         {"mastersthesis", "thesis"},
        {"thesis", "thesis"},              {"techreport", "report"},             {"report", "report"},
        {"online", "webpage"},             {"electronic", "webpage"},            {"www", "webpage"},
        {"software", "computerProgram"},   {"unpublished", "manuscript"},        {"patent", "patent"},
        {"dataset", "dataset"},            {"data", "dataset"},                  {"preprint", "preprint"},
        {"standard", "standard"},          {"video", "videoRecording"},          {"movie", "film"},
        {"audio", "audioRecording"},       {"misc", "document"}};
    auto it = m.find(t);
    return it == m.end() ? "document" : it->second;
}

// arXiv id from eprint fields or Google Scholar / DBLP style journal fields.
std::string arxivIdOf(const Entry& e)
{
    auto eprint = rawValue(e.get("eprint"));
    if (startsWith(toLower(eprint), "arxiv:"))
        eprint = trim(eprint.substr(6));
    auto prefix = toLower(rawValue(e.get("archiveprefix").empty() ? e.get("eprinttype") : e.get("archiveprefix")));
    if (!eprint.empty() && (prefix == "arxiv" || (prefix.empty() && isArxivId(eprint))))
        return isArxivId(eprint) ? stripArxivVersion(eprint) : std::string();
    auto journal = toLower(decodeText(e.get("journal")));
    if (startsWith(journal, "arxiv")) {
        auto words = split(journal, ' ');
        auto id = words.empty() ? std::string() : words.back();
        if (startsWith(id, "arxiv:"))
            id = id.substr(6);
        if (isArxivId(id))
            return stripArxivVersion(id);
    }
    if (journal == "corr") {
        auto volume = rawValue(e.get("volume"));
        if (startsWith(volume, "abs/") && isArxivId(volume.substr(4)))
            return stripArxivVersion(volume.substr(4));
    }
    return {};
}

std::string bibPages(std::string_view raw)
{
    auto s = replaceAll(std::string(raw), "---", "-");
    s = replaceAll(s, "--", "-");
    s = replaceAll(decodeText(s), "–", "-");
    return s;
}

Json entryToItem(const Entry& e)
{
    auto type = mapEntryType(e.type);
    auto arxivId = arxivIdOf(e);
    auto journal = toLower(decodeText(e.get("journal").empty() ? e.get("journaltitle") : e.get("journal")));
    bool journalIsArxiv = journal.empty() || startsWith(journal, "arxiv") || journal == "corr";
    if (!arxivId.empty() && (type == "document" || type == "manuscript" || type == "webpage" ||
                             (type == "journalArticle" && journalIsArxiv)))
        type = "preprint";
    auto howpublished = e.get("howpublished");
    bool howUrl = howpublished.find("\\url") != std::string::npos || startsWith(trim(howpublished), "http");
    if (type == "document" && howUrl && e.get("publisher").empty())
        type = "webpage";

    Json item = newItem(type);
    std::string year, month, day, date;
    std::vector<std::string> notes, files;
    for (auto& [name, raw] : e.fields) {
        if (name == "author" || name == "editor" || name == "translator" || name == "bookauthor") {
            static const std::map<std::string, std::string> roles = {
                {"author", "author"}, {"editor", "editor"}, {"translator", "translator"}, {"bookauthor", "bookAuthor"}};
            for (auto& n : splitNames(raw)) {
                auto c = parseBibName(n);
                if (!c.is_null())
                    addCreator(item, roles.at(name), c);
            }
        } else if (name == "title") {
            setField(item, "title", decodeText(raw));
        } else if (name == "journal" || name == "journaltitle") {
            if (type != "preprint")
                setField(item, "publicationTitle", decodeText(raw));
        } else if (name == "booktitle") {
            if (!setField(item, "publicationTitle", decodeText(raw)) && jstr(item, "title").empty())
                setField(item, "title", decodeText(raw));
        } else if (name == "eventtitle") {
            if (!setField(item, "conferenceName", decodeText(raw)))
                setField(item, "meetingName", decodeText(raw));
        } else if (name == "shortjournal" || name == "journalabbr") {
            setField(item, "journalAbbreviation", decodeText(raw));
        } else if (name == "series") {
            if (!setField(item, "series", decodeText(raw)))
                setField(item, "seriesTitle", decodeText(raw));
        } else if (name == "volume") {
            if (!(type == "preprint" && startsWith(rawValue(raw), "abs/")))
                setField(item, "volume", decodeText(raw));
        } else if (name == "number") {
            auto v = decodeText(raw);
            bool isIssue = type == "journalArticle" || type == "magazineArticle" || type == "newspaperArticle" ||
                           type == "conferencePaper";
            if (!(isIssue && setField(item, "issue", v)) && !setField(item, "number", v) && !setField(item, "issue", v))
                setField(item, "seriesNumber", v);
        } else if (name == "issue") {
            setField(item, "issue", decodeText(raw));
        } else if (name == "pages") {
            setField(item, "pages", bibPages(raw));
        } else if (name == "pagetotal") {
            setField(item, "numPages", decodeText(raw));
        } else if (name == "year") {
            year = decodeText(raw);
        } else if (name == "month") {
            month = decodeText(raw);
        } else if (name == "day") {
            day = decodeText(raw);
        } else if (name == "date") {
            date = decodeText(raw);
        } else if (name == "publisher" || name == "school" || name == "institution" || name == "organization") {
            if (getField(item, "publisher").empty())
                setField(item, "publisher", decodeText(raw));
        } else if (name == "address" || name == "location") {
            setField(item, "place", decodeText(raw));
        } else if (name == "edition") {
            setField(item, "edition", decodeText(raw));
        } else if (name == "isbn") {
            setField(item, "ISBN", decodeText(raw));
        } else if (name == "issn") {
            setField(item, "ISSN", decodeText(raw));
        } else if (name == "doi") {
            auto doi = cleanDoi(rawValue(raw));
            setField(item, "DOI", doi.empty() ? rawValue(raw) : doi);
        } else if (name == "url") {
            setField(item, "url", rawValue(raw));
        } else if (name == "urldate") {
            auto d = parseDate(rawValue(raw));
            setField(item, "accessDate", d.valid() ? isoDate(d) : rawValue(raw));
        } else if (name == "abstract") {
            setField(item, "abstractNote", decodeParagraphs(raw));
        } else if (name == "keywords") {
            for (auto& part : split(replaceAll(decodeText(raw), ";", ","), ','))
                addTag(item, part);
        } else if (name == "note") {
            appendExtra(item, decodeParagraphs(raw));
        } else if (name == "annote" || name == "annotation") {
            auto text = decodeParagraphs(raw);
            if (!text.empty())
                notes.push_back(textToNoteHtml(text));
        } else if (name == "file") {
            for (auto& f : parseFileField(raw))
                files.push_back(f);
        } else if (name == "language" || name == "langid") {
            if (jstr(item, "language").empty())
                setField(item, "language", decodeText(raw));
        } else if (name == "type") {
            setField(item, "type", decodeText(raw));
        } else if (name == "howpublished") {
            if (howUrl) {
                if (jstr(item, "url").empty())
                    setField(item, "url", rawValue(raw));
            } else {
                appendExtra(item, "Published: " + decodeText(raw));
            }
        } else if (name == "shorttitle") {
            setField(item, "shortTitle", decodeText(raw));
        } else if (name == "pmid") {
            if (!setField(item, "PMID", rawValue(raw)))
                appendExtra(item, "PMID: " + rawValue(raw));
        } else if (name == "pmcid") {
            if (!setField(item, "PMCID", rawValue(raw)))
                appendExtra(item, "PMCID: " + rawValue(raw));
        } else if (name == "version") {
            setField(item, "versionNumber", decodeText(raw));
        } else if (name == "copyright") {
            setField(item, "rights", decodeText(raw));
        } else if (name == "lccn" || name == "callnumber") {
            setField(item, "callNumber", decodeText(raw));
        }
    }

    if (!date.empty()) {
        auto d = parseDate(date);
        setField(item, "date", d.valid() ? isoDate(d) : date);
    } else if (!year.empty()) {
        DateParts d;
        auto y = yearOf(year);
        d.year = y.empty() ? 0 : std::stoi(y);
        if (!month.empty()) {
            d.month = monthFromName(month);
            if (!d.month && allDigits(month) && month.size() <= 2)
                d.month = std::stoi(month);
            if (d.month < 1 || d.month > 12)
                d.month = 0;
        }
        if (d.month && allDigits(day) && day.size() <= 2)
            d.day = std::stoi(day);
        setField(item, "date", d.valid() ? isoDate(d) : year);
    }

    if (type == "thesis" && jstr(item, "thesisType").empty()) {
        if (e.type == "phdthesis")
            setField(item, "thesisType", "PhD thesis");
        else if (e.type == "mastersthesis")
            setField(item, "thesisType", "Master's thesis");
    }

    if (!arxivId.empty()) {
        if (type == "preprint") {
            if (getField(item, "publisher").empty())
                setField(item, "repository", "arXiv");
            setField(item, "archiveID", "arXiv:" + arxivId);
            if (jstr(item, "DOI").empty())
                setField(item, "DOI", "10.48550/arXiv." + arxivId);
            if (jstr(item, "url").empty())
                setField(item, "url", "https://arxiv.org/abs/" + arxivId);
        } else if (extraValue(jstr(item, "extra"), "arXiv").empty()) {
            appendExtra(item, "arXiv: " + arxivId);
        }
    }

    if (!e.key.empty())
        setField(item, "citationKey", e.key);
    if (!notes.empty())
        item["_notes"] = notes;
    if (!files.empty())
        item["_files"] = files;
    return finishItem(item);
}

// =================================================================================
// Export
// =================================================================================

std::string texEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\textbackslash{}"; break;
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '&': case '%': case '$': case '#': case '_':
            out += '\\';
            out += c;
            break;
        case '~': out += "\\textasciitilde{}"; break;
        case '^': out += "\\textasciicircum{}"; break;
        case '\r': break;
        default: out += c;
        }
    }
    return out;
}

// Brace words whose capitalisation must survive BibTeX styles: acronyms, "iPhone", "3D",
// and single capitals after the first word ("Part A").
std::string protectCase(const std::string& escaped)
{
    auto words = split(escaped, ' ');
    for (size_t w = 0; w < words.size(); ++w) {
        auto& word = words[w];
        size_t firstLetter = word.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
        if (firstLetter == std::string::npos)
            continue;
        bool innerUpper = false;
        int letters = 0;
        for (size_t i = 0; i < word.size(); ++i) {
            if (isAlpha(word[i]))
                ++letters;
            if (i > firstLetter && word[i] >= 'A' && word[i] <= 'Z')
                innerUpper = true;
        }
        bool singleCapital = w > 0 && letters == 1 && word[firstLetter] >= 'A' && word[firstLetter] <= 'Z';
        if (innerUpper || singleCapital)
            word = "{" + word + "}";
    }
    return join(words, " ");
}

std::string longText(const std::string& s)
{
    // BibTeX joins lines; a blank line keeps paragraphs apart.
    return texEscape(replaceAll(replaceAll(s, "\r", ""), "\n", "\n\n"));
}

std::string bibName(const Json& c)
{
    auto name = jstr(c, "name");
    if (!name.empty())
        return "{" + texEscape(name) + "}";
    auto last = texEscape(jstr(c, "lastName"));
    auto first = texEscape(jstr(c, "firstName"));
    if (iequals(last, "and") || last.find(" and ") != std::string::npos || last.find(',') != std::string::npos)
        last = "{" + last + "}";
    return first.empty() ? last : last + ", " + first;
}

std::string bibNames(const Json& item, const std::string& creatorType)
{
    std::vector<std::string> names;
    for (auto& c : item.value("creators", Json::array()))
        if (jstr(c, "creatorType") == creatorType)
            names.push_back(bibName(c));
    return join(names, " and ");
}

std::string keyPart(std::string_view s)
{
    std::string out;
    for (char c : normalize(s))
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            out += c;
    return out;
}

std::string generatedKey(const Json& item)
{
    static const std::set<std::string> stop = {"a",   "an",   "the",  "on",   "in", "of",  "for",
                                               "and", "to",   "with", "from", "at", "by",  "is",
                                               "are", "as",   "via",  "or",   "into", "der", "die",
                                               "das", "le",   "la",   "les",  "el", "los", "un", "une"};
    const auto primary = schema::primaryCreatorType(jstr(item, "itemType"));
    const Json creators = item.value("creators", Json::array());
    const Json* first = nullptr;
    for (const std::string& t : {primary, std::string("author"), std::string("editor")}) {
        for (auto& c : creators)
            if (jstr(c, "creatorType") == t) {
                first = &c;
                break;
            }
        if (first)
            break;
    }
    if (!first && !creators.empty())
        first = &creators[0];
    std::string key = first ? keyPart(creatorName(*first)) : std::string();
    key += yearOf(schema::getField(item, "date"));
    for (auto& w : split(normalize(schema::getField(item, "title")), ' ')) {
        auto word = keyPart(w);
        if (!word.empty() && !stop.count(word)) {
            key += word;
            break;
        }
    }
    return key.empty() ? "item" : key;
}

std::string explicitKey(const Json& item)
{
    auto k = trim(jstr(item, "citationKey"));
    if (k.empty())
        k = extraValue(jstr(item, "extra"), "Citation Key");
    std::string out;
    for (char c : k)
        if (!isSpace(c) && std::string_view(",{}\"#%'()=\\").find(c) == std::string_view::npos)
            out += c;
    return out;
}

std::string letterSuffix(size_t n)  // 1 -> a, 26 -> z, 27 -> aa
{
    std::string s;
    while (n > 0) {
        --n;
        s.insert(s.begin(), (char)('a' + n % 26));
        n /= 26;
    }
    return s;
}

std::vector<std::string> assignKeys(const std::vector<const Json*>& items)
{
    std::set<std::string> used;
    std::vector<std::string> keys(items.size());
    // explicit keys first so generated keys never steal them
    for (size_t i = 0; i < items.size(); ++i) {
        auto k = explicitKey(*items[i]);
        if (!k.empty() && used.insert(toLower(k)).second)
            keys[i] = k;
    }
    for (size_t i = 0; i < items.size(); ++i) {
        if (!keys[i].empty())
            continue;
        auto base = explicitKey(*items[i]);
        if (base.empty())
            base = generatedKey(*items[i]);
        std::string k = base;
        for (size_t n = 1; used.count(toLower(k)); ++n)
            k = base + letterSuffix(n);
        used.insert(toLower(k));
        keys[i] = k;
    }
    return keys;
}

class EntryWriter {
public:
    void text(const char* name, const std::string& value)
    {
        if (!trim(value).empty())
            fields_.emplace_back(name, "{" + texEscape(trim(value)) + "}");
    }
    void braced(const char* name, const std::string& value)  // already TeX
    {
        if (!trim(value).empty())
            fields_.emplace_back(name, "{" + value + "}");
    }
    void bare(const char* name, const std::string& value)
    {
        if (!value.empty())
            fields_.emplace_back(name, value);
    }
    std::string render(const std::string& type, const std::string& key) const
    {
        std::string out = "@" + type + "{" + key + ",\n";
        for (auto& [n, v] : fields_)
            out += "  " + n + " = " + v + ",\n";
        out += "}\n";
        return out;
    }

private:
    std::vector<std::pair<std::string, std::string>> fields_;
};

std::string arxivIdOfItem(const Json& item)
{
    auto archiveId = getField(item, "number");
    if (startsWith(toLower(archiveId), "arxiv:")) {
        auto words = split(trim(archiveId.substr(6)), ' ');
        if (!words.empty() && isArxivId(words[0]))
            return stripArxivVersion(words[0]);
    }
    auto doi = toLower(jstr(item, "DOI"));
    if (startsWith(doi, "10.48550/arxiv."))
        return jstr(item, "DOI").substr(15);
    auto url = jstr(item, "url");
    auto pos = url.find("arxiv.org/abs/");
    if (pos != std::string::npos && isArxivId(url.substr(pos + 14)))
        return stripArxivVersion(url.substr(pos + 14));
    return {};
}

// "arXiv:2301.00001 [cs.CV]" in Archive ID or Extra.
std::string arxivClassOf(const Json& item)
{
    std::vector<std::string> lines = split(jstr(item, "extra"), '\n');
    lines.push_back(getField(item, "number"));
    for (auto& line : lines) {
        if (!startsWith(toLower(trim(line)), "arxiv:"))
            continue;
        auto open = line.find('['), close = line.find(']');
        if (open != std::string::npos && close != std::string::npos && close > open + 1)
            return trim(line.substr(open + 1, close - open - 1));
    }
    return {};
}

std::string entryType(const Json& item)
{
    auto type = jstr(item, "itemType");
    if (type == "journalArticle")
        return "article";
    if (type == "conferencePaper")
        return "inproceedings";
    if (type == "book")
        return "book";
    if (type == "bookSection")
        return "incollection";
    if (type == "thesis") {
        auto t = toLower(jstr(item, "thesisType"));
        return t.find("master") != std::string::npos || startsWith(t, "ms") || startsWith(t, "m.s") ||
                       startsWith(t, "m.a") || startsWith(t, "ma ")
                   ? "mastersthesis"
                   : "phdthesis";
    }
    if (type == "report")
        return "techreport";
    if (type == "computerProgram")
        return "software";
    if (type == "manuscript")
        return "unpublished";
    return "misc";
}

std::string itemToBibtex(const Json& item, const std::string& key)
{
    static const char* const kMonthMacros[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                               "jul", "aug", "sep", "oct", "nov", "dec"};
    auto type = jstr(item, "itemType");
    auto bibType = entryType(item);
    auto field = [&](const char* f) { return getField(item, f); };
    auto arxiv = type == "preprint" ? arxivIdOfItem(item) : std::string();
    // URLs are written verbatim; only braces would break the entry
    auto url = replaceAll(replaceAll(jstr(item, "url"), "{", "%7B"), "}", "%7D");
    EntryWriter w;

    w.braced("title", protectCase(texEscape(trim(field("title")))));
    w.braced("author", bibNames(item, schema::primaryCreatorType(type)));
    w.braced("editor", bibNames(item, "editor"));
    w.braced("translator", bibNames(item, "translator"));

    auto container = field("publicationTitle");
    if (bibType == "article" || type == "magazineArticle" || type == "newspaperArticle")
        w.text("journal", container);
    else if (bibType == "inproceedings" || bibType == "incollection" || type == "encyclopediaArticle" ||
             type == "dictionaryEntry")
        w.text("booktitle", container);
    w.text("eventtitle", jstr(item, "conferenceName"));
    w.text("series", field("series").empty() ? field("seriesTitle") : field("series"));
    w.text("volume", field("volume"));
    if (!jstr(item, "issue").empty())
        w.text("number", jstr(item, "issue"));
    else if (arxiv.empty() && !field("number").empty())
        w.text("number", field("number"));
    else if (!jstr(item, "seriesNumber").empty())
        w.text("number", jstr(item, "seriesNumber"));
    auto pages = field("pages");
    for (const char* dash : {"–", "—"})
        pages = replaceAll(pages, dash, "-");
    w.text("pages", replaceAll(replaceAll(pages, "--", "-"), "-", "--"));
    w.text("edition", jstr(item, "edition"));
    auto publisher = field("publisher");
    if (bibType == "phdthesis" || bibType == "mastersthesis")
        w.text("school", publisher);
    else if (bibType == "techreport")
        w.text("institution", publisher);
    else
        w.text("publisher", publisher);
    w.text("address", field("place"));
    if (type == "thesis" || type == "report")
        w.text("type", field("type"));
    if (type == "webpage" && !url.empty())
        w.braced("howpublished", "\\url{" + url + "}");

    auto rawDate = field("date");
    auto d = parseDate(rawDate);
    w.text("year", d.valid() ? std::to_string(d.year) : yearOf(rawDate));
    if (d.month)
        w.bare("month", kMonthMacros[d.month - 1]);
    if (d.day)
        w.text("date", isoDate(d));

    w.text("isbn", jstr(item, "ISBN"));
    w.text("issn", jstr(item, "ISSN"));
    w.braced("doi", jstr(item, "DOI"));
    w.braced("url", url);
    auto access = parseDate(jstr(item, "accessDate"));
    w.text("urldate", isoDate(access));

    if (!arxiv.empty()) {
        w.text("eprint", arxiv);
        w.text("archivePrefix", "arXiv");
        w.text("primaryClass", arxivClassOf(item));
    }

    w.braced("abstract", longText(trim(jstr(item, "abstractNote"))));
    std::vector<std::string> tags;
    for (auto& t : item.value("tags", Json::array()))
        if (!jstr(t, "tag").empty())
            tags.push_back(jstr(t, "tag"));
    w.text("keywords", join(tags, ", "));
    w.text("language", jstr(item, "language"));

    std::vector<std::string> extra;
    for (auto& line : split(jstr(item, "extra"), '\n'))
        if (!startsWith(toLower(trim(line)), "citation key:"))
            extra.push_back(line);
    w.braced("note", longText(trim(join(extra, "\n"))));

    std::vector<std::string> notes;
    for (auto& n : item.value("_notes", Json::array()))
        if (n.is_string())
            notes.push_back(htmlToText(n.get<std::string>()));
    w.braced("annote", longText(trim(join(notes, "\n"))));

    auto escapeFilePart = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '\\' || c == ':' || c == ';')
                out += '\\';
            out += c;
        }
        return out;
    };
    std::vector<std::string> files;
    for (auto& a : item.value("_attachments", Json::array())) {
        auto path = jstr(a, "path");
        if (!path.empty())
            files.push_back(escapeFilePart(jstr(a, "title")) + ":" + escapeFilePart(path) + ":" +
                            escapeFilePart(jstr(a, "contentType")));
    }
    w.braced("file", join(files, ";"));

    return w.render(bibType, key);
}

} // namespace

std::string latexToUnicode(std::string_view latex) { return LatexDecoder(latex).decode(); }

std::string exportBibtex(const std::vector<Json>& items)
{
    std::vector<const Json*> regular;
    for (auto& item : items)
        if (isExportable(item))
            regular.push_back(&item);
    auto keys = assignKeys(regular);
    std::string out;
    for (size_t i = 0; i < regular.size(); ++i) {
        if (i)
            out += '\n';
        out += itemToBibtex(*regular[i], keys[i]);
    }
    return out;
}

std::vector<Json> importBibtex(const std::string& text)
{
    std::vector<Json> out;
    for (auto& e : Parser(text).parse())
        out.push_back(entryToItem(e));
    return out;
}

} // namespace on::formats
