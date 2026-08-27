# The shape of OpenSSL::SSL::SSLSocket, without a network.
#
# The point being pinned is that it is NOT an IO -- in CRuby it is an ordinary
# object with Buffering and SocketForwarder mixed in, and the handle lives
# behind #to_io. Everything that waits on one has to go through that protocol,
# which is why IO.select learned it.
require "socket"
require "openssl"

p OpenSSL::SSL::VERIFY_NONE
p OpenSSL::SSL::VERIFY_PEER

ctx = OpenSSL::SSL::SSLContext.new
p ctx.verify_mode == OpenSSL::SSL::VERIFY_PEER
p ctx.verify_hostname
ctx.verify_mode = OpenSSL::SSL::VERIFY_NONE
p ctx.verify_mode
# set_params is what Net::HTTP calls; it restores the verifying defaults.
ctx.set_params
p ctx.verify_mode == OpenSSL::SSL::VERIFY_PEER

r, w = IO.pipe
ssl = OpenSSL::SSL::SSLSocket.new(r)

# Not an IO, and the handle is behind #to_io.
p ssl.is_a?(IO)
p ssl.to_io.equal?(r)
p ssl.context.verify_mode == OpenSSL::SSL::VERIFY_PEER

ssl.hostname = "example.test"
p ssl.hostname

# Nothing is decrypted before a handshake, and the accessors answer rather
# than raising.
p ssl.pending
p ssl.ssl_version
p ssl.peer_subject
p ssl.sysclose.nil?

# Reading or writing before #connect is an SSLError, not a segfault through a
# handle that names nothing.
begin
  ssl.syswrite("x")
rescue OpenSSL::SSL::SSLError => e
  puts "write: #{e.message}"
end
begin
  ssl.sysread(16)
rescue OpenSSL::SSL::SSLError => e
  puts "read: #{e.message}"
end

# The tie-in: IO.select waits on the SSLSocket through #to_io, which is the
# whole reason the protocol had to work. Nothing is written, so it is not ready.
p IO.select([ssl], nil, nil, 0).nil?
w.write("x")
w.flush
got = IO.select([ssl], nil, nil, 1)
p got.nil?
# The element handed back is the SSLSocket, not the pipe behind it.
p got[0][0].equal?(ssl)

r.close
w.close
