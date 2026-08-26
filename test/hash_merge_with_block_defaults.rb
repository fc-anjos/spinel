# Hash#merge with a conflict block returns a fresh hash, which must inherit
# the receiver's default value and default proc.
left = { "a" => 1, "b" => "left" }
left.default = "missing"
value_result = left.merge({ "a" => 2, "c" => "other" }) { |key, old, new| old }
p value_result["absent"]
p value_result["a"]

suffix = "!"
proc_left = { "a" => 1, "b" => "left" }
proc_left.default_proc = proc { |hh, key| hh[key] = key + suffix }
proc_result = proc_left.merge({ "a" => 2, "c" => "other" }) { |key, old, new| new }
GC.start
p proc_result["absent"]
p proc_result["a"]
p proc_result.length

# Exercise the dedicated PolyPolyHash block-form merge path too.
poly_left = Hash.new { |hh, key| "poly:" + key.to_s }
poly_left[1] = "one"
poly_right = Hash.new { |hh, key| "other:" + key.to_s }
poly_right[1] = "other"
poly_result = poly_left.merge(poly_right) { |key, old, new| old }
p poly_result[99]
p poly_result[1]
