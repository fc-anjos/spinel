# IO.select waits on anything that answers #to_io, which is how CRuby lets a
# wrapper -- a protocol object holding a socket -- be waited on. Spinel took
# only a real IO and raised TypeError on everything else, so an object of the
# shape OpenSSL::SSL::SSLSocket has (not an IO; answers #to_io) could not be
# handed to an event loop at all.
#
# The runtime cannot dispatch a user method, so codegen emits a cls_id switch
# and main() installs it through sp_user_to_io_hook -- the same shape as the
# exception-parent and JSON hooks. Nothing in a program names #to_io, so it is
# a reachability root like #to_path.
require "socket"

class Wrapper
  def initialize(io)
    @io = io
  end
  def to_io
    @io
  end
end

r, w = IO.pipe
wr = Wrapper.new(r)

# Nothing written yet: the wrapper is watched, and is not ready.
p IO.select([wr], nil, nil, 0).nil?

w.write("hi")
w.flush
got = IO.select([wr], nil, nil, 1)
p got.nil?
p got[0].length
# The element handed back is the WRAPPER, not the IO behind it, as CRuby does.
p got[0][0].equal?(wr)
p r.read_nonblock(2)

# The write side answers through a wrapper too.
ww = Wrapper.new(w)
ready = IO.select(nil, [ww], nil, 1)
p ready.nil?
p ready[1][0].equal?(ww)

# A real IO still works alongside a wrapper in the same call.
w.write("x")
w.flush
mixed = IO.select([wr, r], nil, nil, 1)
p mixed[0].length

r.close
w.close

# An object that answers nothing is still the TypeError it always was.
class NotAnIo
end
begin
  IO.select([NotAnIo.new], nil, nil, 0)
rescue TypeError => e
  puts "TypeError: #{e.message}"
end
