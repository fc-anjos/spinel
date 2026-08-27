# A pattern's `*rest` bound from a poly scrutinee that holds a typed array.
def m(v)
  case v
  in [f, *r] then "#{f} +#{r.inspect}"
  in Integer then "int"
  end
end
puts m([1, 2, 3])
puts m([1.5, 2.5])
puts m(%w[a b c])
puts m([1, "a", nil])
puts m(7)

def tail(v)
  case v
  in [*init, last] then "#{init.inspect} #{last}"
  in Integer then "int"
  end
end
puts tail([1, 2, 3])
puts tail(%w[x y])

def mid(v)
  case v
  in [a, *m, z] then "#{a} #{m.inspect} #{z}"
  in Integer then "int"
  end
end
puts mid([1, 2, 3, 4])
puts mid([1.0, 2.0])

def find(v)
  case v
  in [*pre, 3, *post] then "#{pre.inspect} | #{post.inspect}"
  in Integer then "int"
  end
end
puts find([1, 2, 3, 4, 5])
puts find([3])
