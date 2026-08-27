# A hash-typed slot holding nil is a NULL pointer, and boxing it into a poly
# wrapped that NULL in a truthy SP_TAG_OBJ instead of answering nil. So
# `h.nil?` was false for a hash that WAS nil, `unless h` fell through, and the
# first read of it dereferenced NULL -- a segfault with a silent-corruption
# stage in front of it. (matz/spinel#4134, reduced from #4132)
#
# What makes the slot nil is an omitted optional parameter, which is why an
# explicit `nil` at the same call site behaved differently from omitting it:
# both paths have to box the same NULL.
class Req
  def initialize(method, path, initheader = nil)
    @method = method
    @path = path
    @headers = {}
    unless initheader.nil?
      initheader.each { |k, v| @headers[k.to_s.downcase] = v.to_s }
    end
  end
  def path = @path
  def [](n) = @headers[n.to_s.downcase]
  def header_nil? = @headers.empty?
end

class Post < Req
  def initialize(path, initheader = nil)
    super("POST", path, initheader)
  end
end

# The omitted form and the hash form in one program: the second call site is
# what gives the parameter a hash type, and the first is what puts NULL in it.
a = Post.new("/f")
b = Post.new("/a", { "content-type" => "text/plain" })
p a.path
p b.path
p b["content-type"]
p a["content-type"]
p a.header_nil?
p b.header_nil?

# Passing the default explicitly has to agree with omitting it.
c = Post.new("/x", nil)
p c.path
p c["content-type"]
p c.header_nil?

# The predicate itself, on a nil hash reaching a poly parameter.
def peek(h)
  return "nil" if h.nil?
  "hash:#{h.size}"
end
p peek(nil)
p peek({ "a" => "1" })

# A nil hash in an array, which boxes through the same path.
box = [nil, { "k" => "v" }]
p box[0].nil?
p box[1].nil?
p box[1]["k"]
