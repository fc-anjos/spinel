# Hash#merge between incompatible specialized layouts folds through the
# universal PolyPoly merge instead of reading the argument through the
# wrong struct (#3261).
merged = { 14 => nil }.merge({ 39 => 1 })
p merged.keys.sort
p merged[14]
p merged[39]
p({ 1 => 2 }.merge({ "a" => "b" }))
p({ a: 1 }.merge({ "x" => 2 }))
p({ 1 => 2 }.merge({ 3 => "s" }))

# The universal result inherits the receiver's default.
left = { 1 => "a" }
left.default = "missing"
result = left.merge({ 1 => 9, 2 => 3 })
p result[1]
p result[2]
p result[99]
p left.default

# An evidence-free empty literal is still safe and returns a fresh copy.
empty = { 14 => nil }.merge({})
p empty.length
p empty.keys
p empty[14]
