# puts / print / p evaluate every argument before writing any.
def a; puts "eval-a"; 1; end
def b; puts "eval-b"; 2; end
p a, b
puts a, b
print a, b
puts
begin
  puts 1, 2, (raise "x")
rescue
  puts "rescued: nothing printed before"
end
i = 0
puts (i += 1), (i += 1), (i += 1)
p i, (i = 10), i
def z = 0
puts [], z
puts [1, []], z
puts *[[]]
puts *[]
arr = [1, 2]
puts *arr, z
p *arr, z
print *arr, z
puts
x = puts("a", "b".upcase)
p x

# an argument whose emitter hoists setup into the statement prelude (a call
# on a user-class receiver) still evaluates in argument order
require "set"
st = Set[1]
p st.size, st.add(2).size, st.size
class Counter
  def initialize; @n = 0; end
  def bump; @n += 1; self; end
  def n; @n; end
end
c = Counter.new
p c.n, c.bump.n, c.n
puts c.n, c.bump.n
n = 0
p n, (n += 1), n
a = [1]
p a.size, a.push(2).size
h = { a: 1 }
p h.size, h.store(:b, 2), h.size
