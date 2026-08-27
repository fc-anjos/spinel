# A missed element read from an int container boxes as nil, not as an Integer.
def klass(v) = v.class.name
def isnil(v) = v.nil?

a = [1, 2]
h = { "a" => 1 }
ih = { 1 => 2 }
fa = [1.5]

p klass(a[5]), klass(a[0])
p klass(h["zz"]), klass(h["a"])
p klass(ih[3]), klass(ih[1])
p klass([].max), klass([].min), klass(a.max)
p klass(a.find { false }), klass(a.detect { |v| v == 2 })
p klass([].first), klass([].last), klass(a.first)
p klass(a.at(9)), klass([[1]].dig(0, 5))
p klass(fa[3]), klass(fa[0])
p klass(Integer("x", exception: false)), klass(Integer("7", exception: false))
p klass(Float("x", exception: false)), klass(Float("1.5", exception: false))
p isnil(a[5]), isnil(a[0]), isnil("s")

x = h["zz"]
y = a[1]
p isnil(x), isnil(y), klass(x), klass(y)

def pick(arr, i) = arr[i]
p klass(pick(a, 7)), klass(pick(a, 0))

out = []
out << a[5] << a[0] << h["q"]
p out
p [a[9], h["zz"], a[0]]
