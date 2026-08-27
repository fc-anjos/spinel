# A bare `next` yields nil, even when the block's tail is typed.
p [1, 2, 3].map { |v| next if v == 2; v }
p ["a", "b"].map { |v| next if v == "a"; v }
p [1.5, 2.5].map { |v| next if v > 2; v }
p 5.times.map { |v| next if v.even?; v }
p [1, 2, 3].map { |v| next if v == 2; v }.compact
p({ a: 1, b: 2 }.map { |k, v| next if v == 1; k })

def each_val
  yield 1
  yield 2
end
r = []
each_val { |v| next if v == 1; r << v }
p r

f = proc { |x| next if x; :kept }
p f.call(true), f.call(false)
p (1..4).map { |v| next if v == 3; v * 2 }
p 1.upto(3).map { |v| next if v == 1; v }
