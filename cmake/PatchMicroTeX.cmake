# Fixes to MicroTeX, made to its sources where they are (downloaded, or a local copy) at
# configure time. Each is a piece of its code and what replaces it: already there, it is
# left as it is; neither there, configuring stops (MicroTeX changed: look again). A piece
# that is there more than once is replaced everywhere.
#
# MicroTeX crashed (a null pointer, mostly) on formulas with nothing where it expects
# something - \limits with nothing before it, \textstyle, \mathbb{ }, \smash{ } or \rlap{ }
# with nothing after or in it, \multicolumn outside a table, & or \\ in a {group} in a
# matrix, an empty line in a gather - and it hung on a command defined by itself or on
# something too big to draw. A formula typed in a note took the app down with it. Found by
# rendering tens of thousands of random formulas (random tokens, and nested groups,
# environments, scripts), then by going through its commands one by one.

function(ournotes_microtex_fix file before after)
    set(path "${microtex_SOURCE_DIR}/${file}")
    file(READ "${path}" text)
    string(FIND "${text}" "${after}" done)
    if(NOT done EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${before}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "PatchMicroTeX: ${file} has changed; see cmake/PatchMicroTeX.cmake")
    endif()
    string(REPLACE "${before}" "${after}" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()

# ---- Nothing where something was expected: an empty atom in its place

# \limits, \nolimits with nothing before them
ournotes_microtex_fix(src/core/macro_impl.h [=[
  auto atom = tp.popLastAtom();
  auto copy = atom->clone();]=] [=[
  auto atom = tp.popLastAtom();
  if (atom == nullptr) throw ex_parse("Nothing before \\limits or \\nolimits");  // OurNotes
  auto copy = atom->clone();]=])

# \shoveright{ }, \shoveleft{ }
ournotes_microtex_fix(src/core/macro_impl.h [=[
  auto a = Formula(tp, args[1])._root;
  a->_alignment = Alignment::]=] [=[
  auto a = Formula(tp, args[1])._root;
  if (a == nullptr) a = sptrOf<EmptyAtom>();  // OurNotes
  a->_alignment = Alignment::]=])

# a style (\displaystyle...) with nothing to style
ournotes_microtex_fix(src/atom/atom_impl.h [=[
    _style = style;
    _at = a;
    _type = a->_type;]=] [=[
    _style = style;
    _at = a == nullptr ? sptrOf<EmptyAtom>() : a;  // OurNotes
    _type = _at->_type;]=])

# a text style (\mathbb{ }, \mathrm{ }...) with nothing in it
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  TextStyleAtom(const sptr<Atom>& a, std::string style) : _style(std::move(style)), _at(a) {}]=] [=[
  TextStyleAtom(const sptr<Atom>& a, std::string style)
      : _style(std::move(style)), _at(a == nullptr ? sptrOf<EmptyAtom>() : a) {}  // OurNotes]=])

# \raisebox with nothing in it
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  ) : _base(base), _ru(ru), _r(r), _hu(hu), _h(h), _du(du), _d(d) {}]=] [=[
  ) : _base(base == nullptr ? sptrOf<EmptyAtom>() : base),  // OurNotes
        _ru(ru), _r(r), _hu(hu), _h(h), _du(du), _d(d) {}]=])

# \c{ }, \k{ }, \rlap{ } (and \llap, \clap...), \reflectbox{ }, \textsc{ }, \mathsf{ },
# \st{ }, \mathtt{ } - and {\sc }, {\sf }, {\tt } with nothing after them
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit CedillaAtom(const sptr<Atom>& base) : _base(base) {}]=] [=[
  explicit CedillaAtom(const sptr<Atom>& base)
      : _base(base == nullptr ? sptrOf<EmptyAtom>() : base) {}  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit OgonekAtom(const sptr<Atom>& base) : _base(base) {}]=] [=[
  explicit OgonekAtom(const sptr<Atom>& base)
      : _base(base == nullptr ? sptrOf<EmptyAtom>() : base) {}  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  LapedAtom(const sptr<Atom>& a, wchar_t type) : _at(a), _type(type) {}]=] [=[
  LapedAtom(const sptr<Atom>& a, wchar_t type)
      : _at(a == nullptr ? sptrOf<EmptyAtom>() : a), _type(type) {}  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit ReflectAtom(const sptr<Atom>& base) : _base(base) {]=] [=[
  explicit ReflectAtom(const sptr<Atom>& base)
      : _base(base == nullptr ? sptrOf<EmptyAtom>() : base) {  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit SmallCapAtom(const sptr<Atom>& base) : _base(base) {}]=] [=[
  explicit SmallCapAtom(const sptr<Atom>& base)
      : _base(base == nullptr ? sptrOf<EmptyAtom>() : base) {}  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit SsAtom(const sptr<Atom>& base) : _base(base) {}]=] [=[
  explicit SsAtom(const sptr<Atom>& base)
      : _base(base == nullptr ? sptrOf<EmptyAtom>() : base) {}  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit StrikeThroughAtom(const sptr<Atom>& a) : _at(a) {}]=] [=[
  explicit StrikeThroughAtom(const sptr<Atom>& a)
      : _at(a == nullptr ? sptrOf<EmptyAtom>() : a) {}  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_impl.h [=[
  explicit TtAtom(const sptr<Atom>& base) : _base(base) {}]=] [=[
  explicit TtAtom(const sptr<Atom>& base)
      : _base(base == nullptr ? sptrOf<EmptyAtom>() : base) {}  // OurNotes]=])

# \resizebox{1cm}{1cm}{ }
ournotes_microtex_fix(src/atom/atom_impl.h [=[
    _type = base->_type;
    _base = base;
    _keepAspectRatio = keepAspectRatio;]=] [=[
    _base = base == nullptr ? sptrOf<EmptyAtom>() : base;  // OurNotes
    _type = _base->_type;
    _keepAspectRatio = keepAspectRatio;]=])

# \rotatebox{90}{ }
ournotes_microtex_fix(src/atom/atom_impl.cpp [=[
  : _angle(0), _option(Rotation::bl), _xunit(UnitType::em), _yunit(UnitType::em), _x(0), _y(0) {
  _type = base->_type;
  _base = base;]=] [=[
  : _angle(0), _option(Rotation::bl), _xunit(UnitType::em), _yunit(UnitType::em), _x(0), _y(0) {
  _base = base == nullptr ? sptrOf<EmptyAtom>() : base;  // OurNotes
  _type = _base->_type;]=])

# \smash{ }
ournotes_microtex_fix(src/atom/atom_basic.h [=[
    sptr<Box> b = _atom->createBox(env);
    if (_h) b->_height = 0;]=] [=[
    if (_atom == nullptr) return sptrOf<StrutBox>(0.f, 0.f, 0.f, 0.f);  // OurNotes
    sptr<Box> b = _atom->createBox(env);
    if (_h) b->_height = 0;]=])

# \scalebox{2}{ }, and a size (\tiny...) with nothing after it
ournotes_microtex_fix(src/atom/atom_basic.h [=[
  ScaleAtom(const sptr<Atom>& base, float sx, float sy) noexcept
    : _base(base), _sx(sx), _sy(sy) {]=] [=[
  ScaleAtom(const sptr<Atom>& base, float sx, float sy) noexcept
    : _base(base == nullptr ? sptrOf<EmptyAtom>() : base), _sx(sx), _sy(sy) {  // OurNotes]=])

# \( \), \[ \], and $ $ in \text{}
ournotes_microtex_fix(src/atom/atom_basic.h [=[
  MathAtom(const sptr<Atom>& base, TexStyle style) noexcept
    : _base(base), _style(style) {}]=] [=[
  MathAtom(const sptr<Atom>& base, TexStyle style) noexcept
    : _base(base == nullptr ? sptrOf<EmptyAtom>() : base), _style(style) {}  // OurNotes]=])

# \mathop{ } (and \mathrel, \mathbin...)
ournotes_microtex_fix(src/atom/atom_basic.h [=[
    : _leftType(lt), _rightType(rt), _atom(atom) {
    _limitsType = atom->_limitsType;]=] [=[
    : _leftType(lt), _rightType(rt), _atom(atom == nullptr ? sptrOf<EmptyAtom>() : atom) {  // OurNotes
    _limitsType = _atom->_limitsType;]=])

# \undertilde{ } next to something: it can draw nothing under nothing, but had no sides
ournotes_microtex_fix(src/atom/atom_basic.h [=[
    _overSmall = oversmall;
  }

  AtomType leftType() const override {
    return _base->leftType();
  }

  AtomType rightType() const override {
    return _base->rightType();
  }]=] [=[
    _overSmall = oversmall;
  }

  AtomType leftType() const override {
    return _base == nullptr ? _type : _base->leftType();  // OurNotes
  }

  AtomType rightType() const override {
    return _base == nullptr ? _type : _base->rightType();  // OurNotes
  }]=])

# a sub- or superscript on an accent with nothing under it (\hat \limits_)
ournotes_microtex_fix(src/atom/atom_basic.cpp [=[
  if (acc != nullptr) {
    // special case: accent]=] [=[
  if (acc != nullptr && acc->_base != nullptr) {  // OurNotes
    // special case: accent]=])

# \multicolumn, \multirow with nothing in them
ournotes_microtex_fix(src/atom/atom_matrix.h [=[
    _n = n >= 1 ? n : 1;
    _cols = cols;]=] [=[
    _n = n >= 1 ? n : 1;
    _cols = cols == nullptr ? sptrOf<EmptyAtom>() : cols;  // OurNotes]=])
ournotes_microtex_fix(src/atom/atom_matrix.h [=[
    : _i(0), _j(0), _rows(rows), _n(n == 0 ? 1 : n) {}]=] [=[
    : _i(0), _j(0), _rows(rows == nullptr ? sptrOf<EmptyAtom>() : rows),  // OurNotes
      _n(n == 0 ? 1 : n) {}]=])

# an empty line in a gather or multline; and none at all
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
  auto* vb = new VBox();
  auto atom = _column->_array[0][0];]=] [=[
  // OurNotes: an empty line has no atom
  if (_column->rows() < 1) return sptrOf<StrutBox>(0.f, 0.f, 0.f, 0.f);
  for (auto& row : _column->_array)
    for (auto& cell : row)
      if (cell == nullptr) cell = sptrOf<EmptyAtom>();
  auto* vb = new VBox();
  auto atom = _column->_array[0][0];]=])

# a character with no metrics or extension: "nothing" + 1 is not nothing (a delimiter that
# cannot grow was taken for one that can)
ournotes_microtex_fix(src/fonts/font_info.cpp [=[
  return _metrics.isEmpty() ? nullptr : _metrics((float)ch) + 1;]=] [=[
  if (_metrics.isEmpty()) return nullptr;  // OurNotes
  auto found = _metrics((float)ch);
  return found == nullptr ? nullptr : found + 1;]=])
ournotes_microtex_fix(src/fonts/font_info.cpp [=[
  return _extensions.isEmpty() ? nullptr : _extensions((int)ch) + 1;]=] [=[
  if (_extensions.isEmpty()) return nullptr;  // OurNotes
  auto found = _extensions((int)ch);
  return found == nullptr ? nullptr : found + 1;]=])

# a character after the last in a font's table (its metrics, kerning, ligatures, larger
# sizes): the search read the row past the end, from whatever came next
ournotes_microtex_fix(src/utils/indexed_arr.h [=[
    int     l = 0, h = _rows;]=] [=[
    int     l = 0, h = (int) _rows - 1;  // OurNotes]=])

# \genfrac{}{}{}{9}{a}{b}: a style other than the four (0 to 3), an index past the tables
ournotes_microtex_fix(src/core/macro_impl.cpp [=[
  int style = 0;
  if (!args[4].empty()) valueof(args[4], style);
]=] [=[
  int style = 0;
  if (!args[4].empty()) valueof(args[4], style);
  if (style < 0 || style > 3) throw ex_parse("A style of \\genfrac is 0 to 3");  // OurNotes
]=])

# ---- Tables: a cell, row or line that is not where MicroTeX looks for it

# & or \\ (and the commands of a table) in a {group} in a matrix took the group's plain
# formula for the matrix's, and wrote over memory: a table's only where its formula is one
ournotes_microtex_fix(src/core/parser.cpp [=[
  if (!_arrayMode) throw ex_parse("Can not add row in none-array mode!");]=] [=[
  if (!_arrayMode || !_formula->isArrayMode())  // OurNotes
    throw ex_parse("Can not add row in none-array mode!");]=])
ournotes_microtex_fix(src/core/parser.cpp [=[
        if (_arrayMode && h != nullptr) ((ArrayFormula*) _formula)->addRow();]=] [=[
        if (_arrayMode && h != nullptr && _formula->isArrayMode())  // OurNotes
          ((ArrayFormula*) _formula)->addRow();]=])
ournotes_microtex_fix(src/core/parser.cpp [=[
        if (!_arrayMode) {
          throw ex_parse("Character '&' is only available in array mode!");]=] [=[
        if (!_arrayMode || !_formula->isArrayMode()) {  // OurNotes
          throw ex_parse("Character '&' is only available in array mode!");]=])
ournotes_microtex_fix(src/core/macro_impl.h [=[
  if (!tp.isArrayMode()) throw ex_parse("Command \\cellcolor must used in array environment!");]=] [=[
  if (!tp.isArrayMode() || !tp._formula->isArrayMode())  // OurNotes
    throw ex_parse("Command \\cellcolor must used in array environment!");]=])
ournotes_microtex_fix(src/core/macro_impl.h [=[
  if (!tp.isArrayMode()) throw ex_parse("Command \\rowcolor must used in array environment!");]=] [=[
  if (!tp.isArrayMode() || !tp._formula->isArrayMode())  // OurNotes
    throw ex_parse("Command \\rowcolor must used in array environment!");]=])
ournotes_microtex_fix(src/core/macro_impl.h [=[
inline macro(hdotsfor) {
  if (!tp.isArrayMode())
    throw ex_parse("Command 'hdotsfor' only available in array mode!");
  int n = 0;
  valueof(args[1], n);]=] [=[
inline macro(hdotsfor) {
  if (!tp.isArrayMode() || !tp._formula->isArrayMode())  // OurNotes
    throw ex_parse("Command 'hdotsfor' only available in array mode!");
  int n = 0;
  valueof(args[1], n);
  if (n < 1 || n > 1000) throw ex_parse("A \\hdotsfor spans 1 to 1000 columns");  // OurNotes]=])
ournotes_microtex_fix(src/core/macro_impl.h [=[
inline macro(hline) {
  if (!tp.isArrayMode())]=] [=[
inline macro(hline) {
  if (!tp.isArrayMode() || !tp._formula->isArrayMode())  // OurNotes]=])
ournotes_microtex_fix(src/core/macro_impl.cpp [=[
macro(intertext) {
  if (!tp.isArrayMode())]=] [=[
macro(intertext) {
  if (!tp.isArrayMode() || !tp._formula->isArrayMode())  // OurNotes]=])
# \\ in a {group} in a cell stays a line break in it
ournotes_microtex_fix(src/core/macro_impl.h [=[
inline macro(cr) {
  if (tp.isArrayMode()) {]=] [=[
inline macro(cr) {
  if (tp.isArrayMode() && tp._formula->isArrayMode()) {  // OurNotes]=])
# \multicolumn outside a table: there is no row to add it to. \multicolumn{99999999}, or
# {-9223372036854775808} (one less than the least int, without end): that many cells made
ournotes_microtex_fix(src/core/macro_impl.h [=[
inline macro(multicolumn) {
  int n = 0;
  valueof(args[1], n);]=] [=[
inline macro(multicolumn) {
  if (!tp.isArrayMode() || !tp._formula->isArrayMode())  // OurNotes
    throw ex_parse("Command 'multicolumn' only available in array mode!");
  int n = 0;
  valueof(args[1], n);
  if (n < 1 || n > 1000) throw ex_parse("A \\multicolumn spans 1 to 1000 columns");  // OurNotes]=])

# a column put into rows with fewer cells than its place (\begin{cases} with a row of one)
ournotes_microtex_fix(src/core/formula.cpp [=[
  for (size_t j = 0; j < _row; j++) {
    auto it = _array[j].begin();
    _array[j].insert(it + col, atom);
  }]=] [=[
  for (size_t j = 0; j < _row; j++) {
    auto& cells = _array[j];  // OurNotes: filled up to it first
    if (cells.size() < (size_t) col) cells.resize(col);
    cells.insert(cells.begin() + col, atom);
  }]=])

# a smallmatrix: its smaller copy of the environment was assigned over the one it was given
# (what came after it shrank too), from an object that the assignment freed
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
sptr<Box> MatrixAtom::createBox(Environment& e) {
  Environment& env = e;]=] [=[
sptr<Box> MatrixAtom::createBox(Environment& e) {
  sptr<Environment> small;  // OurNotes
  if (_matType == MatrixType::smallMatrix) {
    small = e.copy();
    small->setStyle(TexStyle::script);
  }
  Environment& env = small != nullptr ? *small : e;]=])
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
  float drt = env.getTeXFont()->getDefaultRuleThickness(env.getStyle());

  if (_matType == MatrixType::smallMatrix) {
    env = *(e.copy());
    env.setStyle(TexStyle::script);
  } /* else if (_matType == MatrixType::matrix) {
    env = *(e.copy());
    env.setStyle(STYLE_TEXT);
  }*/]=] [=[
  float drt = e.getTeXFont()->getDefaultRuleThickness(e.getStyle());  // OurNotes]=])

# a \multirow with scripts is no MultiRowAtom, though it gives the box of one
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
      if (boxarr[i][j]->_type != AtomType::multiRow) {
        // Find the highest line (row)
        lineDepth[i] = max(boxarr[i][j]->_depth, lineDepth[i]);
        lineHeight[i] = max(boxarr[i][j]->_height, lineHeight[i]);
      } else {
        auto* mra = (MultiRowAtom*) atom.get();
        mra->setRowColumn(i, j);]=] [=[
      // OurNotes: a \multirow with scripts gives the box of one, but is none
      auto* mra = dynamic_cast<MultiRowAtom*>(atom.get());
      if (mra == nullptr && boxarr[i][j]->_type == AtomType::multiRow)
        boxarr[i][j]->_type = AtomType::none;
      if (boxarr[i][j]->_type != AtomType::multiRow) {
        // Find the highest line (row)
        lineDepth[i] = max(boxarr[i][j]->_depth, lineDepth[i]);
        lineHeight[i] = max(boxarr[i][j]->_height, lineHeight[i]);
      } else {
        mra->setRowColumn(i, j);]=])

# a \multirow down past the last row
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
      const int mr = m->_i + m->_n;]=] [=[
      const int mr = std::min(m->_i + m->_n, rows);  // OurNotes: not past the last row]=])

# \hline after & under a shorter row
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
          if (i >= 1 && dynamic_cast<HlineAtom*>(_matrix->_array[i - 1][j].get()) != nullptr) {]=] [=[
          if (i >= 1 && j < (int) _matrix->_array[i - 1].size()  // OurNotes
              && dynamic_cast<HlineAtom*>(_matrix->_array[i - 1][j].get()) != nullptr) {]=])

# ---- Without end: commands defined by themselves, things too big to draw

# \newcommand{\newcommand}{}: these four are run before parsing, and broke it as commands
ournotes_microtex_fix(src/core/parser.cpp [=[
bool TeXParser::isValidName(const wstring& com) const {
  if (com.empty()) return false;
  if (com[0] != '\\') return false;]=] [=[
bool TeXParser::isValidName(const wstring& com) const {
  if (com.empty()) return false;
  if (com[0] != '\\') return false;
  if (com == L"\\newcommand" || com == L"\\renewcommand"  // OurNotes
      || com == L"\\newenvironment" || com == L"\\renewenvironment") return false;]=])

# \newcommand{\x}[-1]{}: the arguments were read from before their start (and a command
# defined stays defined, for every formula after it)
ournotes_microtex_fix(src/core/macro_impl.cpp [=[
  if (!args[3].empty()) valueof(args[3], nbArgs);
]=] [=[
  if (!args[3].empty()) valueof(args[3], nbArgs);
  if (nbArgs < 0 || nbArgs > 9) throw ex_parse("A command has 0 to 9 arguments");  // OurNotes
]=])
ournotes_microtex_fix(src/core/macro_impl.h [=[
  int opt = 0;
  if (!args[4].empty()) valueof(args[4], opt);
]=] [=[
  int opt = 0;
  if (!args[4].empty()) valueof(args[4], opt);
  if (opt < 0 || opt > 9) throw ex_parse("An environment has 0 to 9 arguments");  // OurNotes
]=])

# \newcommand{\x}{\x}\x: written out again and again before parsing, without end (or until
# the formula filled the memory)
ournotes_microtex_fix(src/core/parser.cpp [=[
  int spos;
  vector<wstring> args;
  while (_pos < _len) {
    if (replaceScript()) continue;

    ch = _latex[_pos];
    switch (ch) {
      case ESCAPE: {
        spos = _pos;
        wstring cmd = getCommand();
        try {
          preprocess(cmd, args, spos);
        } catch (ex_parse& e) {
          if (!_isPartial) throw;
        }
        args.clear();
        break;]=] [=[
  int spos;
  int expansions = 0;  // OurNotes
  vector<wstring> args;
  while (_pos < _len) {
    if (replaceScript()) continue;

    ch = _latex[_pos];
    switch (ch) {
      case ESCAPE: {
        spos = _pos;
        wstring cmd = getCommand();
        try {
          preprocess(cmd, args, spos);
        } catch (ex_parse& e) {
          if (!_isPartial) throw;
        }
        args.clear();
        // OurNotes: written out where it was, to be read again
        if (_pos <= spos && (++expansions > 1000 || _len > 100000))
          throw ex_parse("Too many commands written out: is one defined by itself?");
        break;]=])

# \a\newcommand{\a}{\frac{\a}{b}}, or x^\a: a command used before it is defined is written
# out while parsing, and so was the same command in it, until the stack ran out. A formula,
# with the formulas in it, writes out so many at most.
ournotes_microtex_fix(src/core/parser.cpp [=[
sptr<Atom> TeXParser::processCommands(const wstring& cmd, MacroInfo* mac) {]=] [=[
namespace {  // OurNotes
thread_local int ourParsing = 0, ourExpansions = 0;
struct OurParsing {
  OurParsing() { if (ourParsing++ == 0) ourExpansions = 0; }
  ~OurParsing() { ourParsing--; }
};
}  // namespace

sptr<Atom> TeXParser::processCommands(const wstring& cmd, MacroInfo* mac) {]=])
ournotes_microtex_fix(src/core/parser.cpp [=[
  if (NewCommandMacro::isMacro(cmd)) {
    // The last value in "args" is the replacement string]=] [=[
  if (NewCommandMacro::isMacro(cmd)) {
    if (++ourExpansions > 200)  // OurNotes
      throw ex_parse("Too many commands written out: is one defined by itself?");
    // The last value in "args" is the replacement string]=])
ournotes_microtex_fix(src/core/parser.cpp [=[
void TeXParser::parse() {
  if (_len == 0) {]=] [=[
void TeXParser::parse() {
  OurParsing parsing;  // OurNotes
  if (_len == 0) {]=])

# \newcolumntype{x}{xx}: a column type written out in itself, without end
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
            opt.insert(spos, it->second);
            len = opt.length();]=] [=[
            opt.insert(spos, it->second);
            len = opt.length();
            if (len > 2000) throw ex_parse("Column types written out too long");  // OurNotes]=])

# \longdiv{-9223372036854775808}{-1}: the quotient does not fit (a trap)
ournotes_microtex_fix(src/core/macro_impl.h [=[
  if (divisor == 0) throw ex_parse("Divisor must not be 0.");]=] [=[
  if (divisor == 0) throw ex_parse("Divisor must not be 0.");
  if (divisor == -1 && dividend == LONG_MIN) throw ex_parse("The quotient is too large.");  // OurNotes]=])

# \sqrt{\scalebox{1e6}x}: a delimiter grown piece by piece, millions of them
ournotes_microtex_fix(src/box/box_factory.cpp [=[
    while (vBox->_height + vBox->_depth <= minHeight) {]=] [=[
    for (int n = 0; n < 1000 && vBox->_height + vBox->_depth <= minHeight; n++) {  // OurNotes]=])

# \overrightarrow{\scalebox{1e8}x}: an arrow made of as many pieces, until the float sum
# stopped growing (both arrows)
ournotes_microtex_fix(src/box/box_factory.cpp [=[
  float w = 0.f;
  for (w = 0; w < width - swidth - mwidth; w += mwidth) {]=] [=[
  float w = 0.f;
  int pieces = 0;  // OurNotes
  for (w = 0; w < width - swidth - mwidth && pieces++ < 1000; w += mwidth) {]=])

# \hdotsfor in a column wide as \scalebox{1e8}x: a dot for every bit of it
ournotes_microtex_fix(src/atom/atom_matrix.cpp [=[
  int count = (int) floor(x);]=] [=[
  int count = (int) floor(std::min(x, 1000.f));  // OurNotes]=])

# ---- Leaks (found with LeakSanitizer)

# A matrix, align, gather or multline environment that does not parse (as one typed in
# place is, a key at a time) threw before anything owned its rows: they leaked at each
# render. Owned from the start; the atom made at the end shares them.
ournotes_microtex_fix(src/core/macro_impl.h [=[sptr<ArrayFormula>(arr)]=] [=[std::move(arrOwner)]=])
ournotes_microtex_fix(src/core/macro_impl.h [=[
  auto* arr = new ArrayFormula();]=] [=[
  auto* arr = new ArrayFormula();
  sptr<ArrayFormula> arrOwner(arr);  // OurNotes]=])

# asprintf's string given back with delete (malloc'ed: free)
ournotes_microtex_fix(src/latex.cpp [=[
    delete userdata_fallback;]=] [=[
    free(userdata_fallback);  // OurNotes]=])
