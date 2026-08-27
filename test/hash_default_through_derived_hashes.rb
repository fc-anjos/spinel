# dup / clone / merge / compact keep the default proc; except drops defaults.
g = Hash.new { |hh, k| hh[k] = k * 2 }
g["a"] = "x"
p g.dup["yy"], g.clone["xx"], g.merge({})["ss"], g.compact["qq"]
p g.merge({ "b" => "y" }).size

s = Hash.new { |hh, k| hh[k] = k.to_s }
s[:a] = "x"
p s.dup[:b], s.clone[:c], s.compact[:d], s.merge({})[:e]

m = Hash.new { |hh, k| hh[k] = [k] }
m[1] = "one"
m["s"] = 2
p m.dup[9], m.clone[8], m.compact[7], m.merge({})[6]

# plain defaults are kept by the same derivations
h = Hash.new(7)
h["a"] = 1
p h.dup["z"], h.clone["z"], h.merge({})["z"], h.compact["z"]

# except drops both the default and the default proc
p h.except("a")["zz"]
c = Hash.new(0)
c["a"] = 2
p c.except("a")["z"]
d = Hash.new("dflt")
d["q"] = "v"
p d.except("q")["nope"]
p g.except("a")["ww"]
p s.except(:a)[:ww]
p m.except(1)[42]
p g.size, s.size, m.size
