# Marshal.load takes either the serialized bytes or an IO to read them from,
# and Marshal.dump writes to an IO when it is given one. Only the String forms
# were compiled: `Marshal.load(f)` handed the sp_File* over as though the
# handle itself were the bytes, and the C build stopped on the pointer type.
# `Marshal.dump(obj, f)` matched no arm at all and came out as a NameError on
# the Marshal constant. (matz/spinel#4112)
path = "/tmp/sp_marshal_io_stream_#{Process.pid}.bin"

# The round trip through a stream, with the value carrying a NUL-heavy dump:
# the write is binary, so the bytes are sized from the header rather than by
# strlen, which would stop at the first NUL inside the payload.
File.open(path, "wb") { |f| Marshal.dump({ "a" => [1, 2.5, nil, true] }, f) }
p File.size(path) > 0
p File.open(path) { |f| Marshal.load(f) }

# The shape it was reduced from: the block's value is untyped, and the result
# lands in an ivar.
class Store
  def load_from(p)
    @table = File.open(p) { |f| Marshal.load(f) }
  end
  attr_reader :table
end
s = Store.new
p s.load_from(path)
p s.table.is_a?(Hash)

# dump answers the stream it wrote to, as CRuby does.
File.open(path, "wb") { |f| p Marshal.dump("x", f).is_a?(File) }

# The String forms are untouched.
bytes = Marshal.dump([1, "two", :three])
p bytes.is_a?(String)
p Marshal.load(bytes)

# A handle held in a local, rather than a block parameter, reaches the same arm.
f = File.open(path, "rb")
p Marshal.load(f)
f.close

File.unlink(path)
