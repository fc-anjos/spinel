# `...` into a callee with `*rest` / `**kwrest` keeps positionals and keywords apart.
def h1(*a, **k) = [a, k]
def h10(...) = h1(...)
p h10(1, k: 2)
p h10(3)
p h10
p h10(1, 2, k: 3, j: 4)

# rest only, kwrest only
def h3(*a) = a
def h30(...) = h3(...)
p h30(1, 2)
p h30
def h4(**k) = k
def h40(...) = h4(...)
p h40(k: 2, j: 3)

# leading concrete params on both sides, and mixed keyword/kwrest
def h5(x, *a, k: 0, **r) = [x, a, k, r]
def h50(x, ...) = h5(x, ...)
p h50(0, 1, 2, k: 5, z: 6)
p h50(0)

# inside a class
class Fw
  def go(*a, **k) = [a, k]
  def fw(...) = go(...)
end
p Fw.new.fw(1, q: 2)

# fixed-arity callees keep the direct positional forward
def h2(a, b: 0) = [a, b]
def h20(...) = h2(...)
p h20(1, b: 5)
p h20(7)
