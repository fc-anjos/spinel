# A counted repeat of a starred atom unrolls to one loop per copy, and every
# copy's body can match empty. Marking those loops walked the rest of the
# program from each one: a*{400}*{400} has 160,000 of them and took a minute
# to compile. Each walk now stays inside its own loop.
re = Regexp.new("a*{400}*{400}")
p re.match?("aaa")
p re.match("baaab")[0]
p Regexp.new("(?:(?:a*){300})*{300}b").match("aab")[0]
p Regexp.new("x(?:y*){200}*{200}z").match?("xyyz")
