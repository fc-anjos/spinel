# A receiverless call is a call on self, so self's own methods come first: the
# enclosing class's singleton chain inside a class method, its instance chain
# inside an instance method. A top-level def is a private method on Object and
# so sits at the bottom of every ancestry. Argument types were bound to the
# top-level method instead whenever one shared the name, which left the real
# callee's parameters to be typed by its own body -- and where the two
# disagreed on a container's shape the C build stopped. Renaming the top-level
# method, or writing the receiver out, made it compile. (matz/spinel#4106)
module Blocks
  def self.read
    state = { "key" => nil }
    title(state)
  end
  def self.title(state) = state["key"] = "v"
end
def title(a) = "top:#{a.size}"
p Blocks.read
p title({ "k" => 1 })

# The same shape one level down: an instance method calling a sibling that
# shares its name with a top-level def.
class Doc
  def run
    state = { "key" => nil }
    helper(state)
  end
  def helper(state) = state["key"] = "inst"
end
def helper(a) = "top:#{a.class}"
p Doc.new.run
p helper([])

# The call really does reach self's method, not the top-level one.
module Counter
  def self.bump = tally(1)
  def self.tally(x) = x + 100
end
def tally(x) = x - 100
p Counter.bump
p tally(1)

# A top-level def is still what a bare call at top level finds, and what an
# object without its own definition of the name falls back to.
def only_at_top(v) = "only:#{v}"
p only_at_top(3)
class Plain
  def ask = only_at_top(4)
end
p Plain.new.ask
