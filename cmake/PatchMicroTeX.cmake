# Fixes to MicroTeX, made to its sources where they are (downloaded, or a local copy) at
# configure time. Each is a piece of its code and what replaces it: already there, it is
# left as it is; neither there, configuring stops (MicroTeX changed: look again).
#
# MicroTeX crashed (a null pointer, mostly) on formulas with nothing where it expects
# something - \limits with nothing before it, \textstyle, \mathbb{ } or \raisebox{1pt}{ }
# with nothing after or in it, \multicolumn outside a table, & or \\ in a {group} in a
# matrix - and a formula typed in a note took the app down with it. Found by rendering tens
# of thousands of random formulas (random tokens, and nested groups, environments, scripts).

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

# \limits, \nolimits with nothing before them
ournotes_microtex_fix(src/core/macro_impl.h
    "  auto atom = tp.popLastAtom();
  auto copy = atom->clone();"
    "  auto atom = tp.popLastAtom();
  if (atom == nullptr) throw ex_parse(\"Nothing before \\\\limits or \\\\nolimits\");  // OurNotes
  auto copy = atom->clone();")

# a style (\displaystyle...) with nothing to style
ournotes_microtex_fix(src/atom/atom_impl.h
    "    _style = style;
    _at = a;
    _type = a->_type;"
    "    _style = style;
    _at = a == nullptr ? sptrOf<EmptyAtom>() : a;  // OurNotes
    _type = _at->_type;")

# a text style (\mathbb{ }, \mathrm{ }...) with nothing in it
ournotes_microtex_fix(src/atom/atom_impl.h
    "  TextStyleAtom(const sptr<Atom>& a, std::string style) : _style(std::move(style)), _at(a) {}"
    "  TextStyleAtom(const sptr<Atom>& a, std::string style)
      : _style(std::move(style)), _at(a == nullptr ? sptrOf<EmptyAtom>() : a) {}  // OurNotes")

# a sub- or superscript on an accent with nothing under it (\hat \limits_)
ournotes_microtex_fix(src/atom/atom_basic.cpp
    "  if (acc != nullptr) {
    // special case: accent"
    "  if (acc != nullptr && acc->_base != nullptr) {  // OurNotes
    // special case: accent")

# \raisebox with nothing in it
ournotes_microtex_fix(src/atom/atom_impl.h
    "  ) : _base(base), _ru(ru), _r(r), _hu(hu), _h(h), _du(du), _d(d) {}"
    "  ) : _base(base == nullptr ? sptrOf<EmptyAtom>() : base),  // OurNotes
        _ru(ru), _r(r), _hu(hu), _h(h), _du(du), _d(d) {}")

# \multicolumn outside a table: there is no row to add it to
ournotes_microtex_fix(src/core/macro_impl.h
    "inline macro(multicolumn) {
  int n = 0;"
    "inline macro(multicolumn) {
  if (!tp.isArrayMode())  // OurNotes
    throw ex_parse(\"Command 'multicolumn' only available in array mode!\");
  int n = 0;")

# a character with no metrics or extension: \"nothing\" + 1 is not nothing (a delimiter that
# cannot grow was taken for one that can)
ournotes_microtex_fix(src/fonts/font_info.cpp
    "  return _metrics.isEmpty() ? nullptr : _metrics((float)ch) + 1;"
    "  if (_metrics.isEmpty()) return nullptr;  // OurNotes
  auto found = _metrics((float)ch);
  return found == nullptr ? nullptr : found + 1;")
ournotes_microtex_fix(src/fonts/font_info.cpp
    "  return _extensions.isEmpty() ? nullptr : _extensions((int)ch) + 1;"
    "  if (_extensions.isEmpty()) return nullptr;  // OurNotes
  auto found = _extensions((int)ch);
  return found == nullptr ? nullptr : found + 1;")

# a {group} in a matrix is no row or cell of it: & or \\ in it took the group's plain formula
# for the matrix's (and wrote over memory)
ournotes_microtex_fix(src/core/parser.cpp
    "    Formula tf;
    Formula* tmp = _formula;
    _formula = &tf;
    _pos++;
    _group++;
    parse();
    _formula = tmp;"
    "    Formula tf;
    Formula* tmp = _formula;
    bool arrayMode = _arrayMode;  // OurNotes
    _formula = &tf;
    _arrayMode = false;
    _pos++;
    _group++;
    parse();
    _formula = tmp;
    _arrayMode = arrayMode;")

# a column put into rows with fewer cells than its place (\begin{cases} with a row of one)
ournotes_microtex_fix(src/core/formula.cpp
    "  for (size_t j = 0; j < _row; j++) {
    auto it = _array[j].begin();
    _array[j].insert(it + col, atom);
  }"
    "  for (size_t j = 0; j < _row; j++) {
    auto& cells = _array[j];  // OurNotes: filled up to it first
    if (cells.size() < (size_t) col) cells.resize(col);
    cells.insert(cells.begin() + col, atom);
  }")

# \multicolumn with nothing in it
ournotes_microtex_fix(src/atom/atom_matrix.h
    "    _n = n >= 1 ? n : 1;
    _cols = cols;"
    "    _n = n >= 1 ? n : 1;
    _cols = cols == nullptr ? sptrOf<EmptyAtom>() : cols;  // OurNotes")
