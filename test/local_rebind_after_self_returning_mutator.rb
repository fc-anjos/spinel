# A local holding both a scalar and a self-returning Array mutator's result stays poly.
x = [1, 2]
r = x.pop
p r
r = x.concat([3])
p r
r = x.push(4)
p r
r = x.reverse!
p r
r = x.sort!
p r
p r.class

# either order
y = [5, 6]
s = 0
s = y.concat([7])
p s
s = y.first
p s

# string arrays too
z = ["a"]
t = z.pop
p t
t = z.concat(["b"])
p t
t = "q"
p t

# an array-only capture still follows the receiver's widening
a = [1, 2]
c = a.push(:x)
p c
p a
