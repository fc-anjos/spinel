# An empty `[]` / `{}` used as a receiver, argument, or interpolation takes
# its kind from that use.

# --- receivers: any method on a bare [] runs over an empty poly array
p([] * 2)
p([] * ",")
p([].dup)
p([].cycle.first(2))
p([].chunk_while { |x, y| x == y }.to_a)
p([].each.class)
p([].reverse_each.class)
p([].inject(:+))
p([].reduce(1, :+))
p([].sum)
p([].max)
p([].assoc(1))

# --- receivers: any method on a bare {} runs over the bare hash
p({}.slice(1))
p({}.except(1))
p({}.values_at(1))
p({}.assoc(1))
p({}.each_with_object([]) { |(k, v), m| m << k })
p({}.reduce(0) { |s, (k, v)| s + v })
p({}.each_pair.to_a)
p({}.each.class)
p({}.delete(:x))

# --- arguments: an empty literal handed to a typed-array method takes that
#     array's kind for the combining methods, and stays a boxed array otherwise
a = [1, 2].sort
b = [1.5].sort
c = %w[x y].sort
a.concat([]); p a
b.concat([]); p b
c.concat([]); p c
p(a + [])
p(a - [])
p(a & [])
p(a | [])
p(a == [])
p(a <=> [])
p(a.union([]))
p(a.difference([]))
p(a.intersect?([]))
p a.include?([])
p a.index([])
p a.rindex([])
p a.find_index([])
p a.member?([])
p b.include?([])
p c.include?([])
p c.index([])
p a.zip([])
p a.product([])
p([[1], [2]].sum([]))
r = [3, 4].sort
r.replace([]); p r

# --- a bare [] receiver combined with a typed array takes that array's kind
p([] + a)
p([] - a)
p([] & a)
p([] | a)
p([].union(a))
p([].intersection(a))
p([].difference(a))
p([] + b)
p([] + c)
p([] == a)
p([] <=> a)

# --- when both operands are bare [], they share the poly-array layout
p([] == [])
p([] != [])
p([] | [])

# --- an empty [] argument on a hash receiver is a poly array
p({}.each_with_object([]) { })
p({"a" => 1}.each_with_object([]) { |(k, v), m| m << k })

# --- the same mismatch with a non-empty array argument
p a.include?([1])
p a.index([1])
p c.include?(1)
p c.index(1)

# --- interpolation
p "#{{}}"
p "#{[]}"
p "x#{{}}y#{[]}z"

# --- `h[k], h[j] = ...` on a bare {} picks the variant from its keys, like
#     `h[k] = v` does
hh = {}
hh[:a], hh[:b] = 1, 2
p hh
hi = {}
hi[1], hi[2] = "x", "y"
p hi

# --- the local back-fill is untouched
x = []
x << 1
p x
p x.sum
y = {}
y["k"] = 2
p y

# --- the seed of an array's each_with_object / inject / reduce is written
#     through the block parameter and back-fills like a local
z = [1, 2, 3].each_with_object([]) { |v, m| m << v * 2 }
p z
p z[0] + 1
p [1, 2, 3].inject([]) { |m, v| m << v }
p [1, 2, 3].reduce([]) { |m, v| m.push(v) }
