# `\K`, `\R`, `\X` and `\p{...}` each mean something here that the engine does
# not do, and as unknown escapes each was its own letter: /\R/ matched an R
# rather than a newline, and /\p{Alpha}/ answered a pattern that asked for a
# letter with the text of the request. Ported from mruby-regexp 2e7e38a80 and
# ef283321f; upstream refuses `\G` and `\g<name>` in the same commit, and both
# are carried here already, so they keep working.
def t(label)
  r = begin
    yield.inspect
  rescue => e
    e.class.to_s
  end
  puts label + " | " + r
end

t("bs-K")      { Regexp.new("a\\Kb") =~ "ab" }
t("bs-R")      { Regexp.new("\\R") =~ "\n" }
t("bs-X")      { Regexp.new("\\X") =~ "a" }

# \G and \g<name> are carried, and keep working
t("bs-G")      { Regexp.new("\\Ga") =~ "a" }
t("bs-G-mid")  { Regexp.new("a\\Gb") =~ "ab" }
t("bs-g-name") { Regexp.new("(?<x>a|b)\\g<x>").match("ab")[0] }
t("bs-g-quote"){ Regexp.new("(?<x>a|b)\\g'x'").match("ab")[0] }
t("prop-p")    { Regexp.new("\\p{Alpha}") =~ "a" }
t("prop-P")    { Regexp.new("\\P{Alpha}") =~ "1" }

# a bare `\g` is the letter in CRuby too, and stays one; so is a bare `\p`
# and the unbraced `\pL`
t("bare-g")    { Regexp.new("\\g").match("g")[0] }
t("bare-p")    { Regexp.new("\\p").match("p")[0] }
t("pL")        { Regexp.new("\\pL").match("pL")[0] }

# inside a character class CRuby reads \R and \K as the letter, and so does
# the class parser here
t("class-R")   { Regexp.new("[\\R]").match("R")[0] }
t("class-K")   { Regexp.new("[\\K]").match("K")[0] }

# \p/\P differ: CRuby reads a property escape inside a class as a property
# test there too, not as its literal letters, so the class parser refuses it
# the same way the bare form is refused rather than matching "p{Alpha}"
t("class-p")     { Regexp.new("[\\p{Alpha}]") =~ "a" }
t("class-P-neg") { Regexp.new("[^\\p{Word}\\- \\t]").match("a d_1") }
