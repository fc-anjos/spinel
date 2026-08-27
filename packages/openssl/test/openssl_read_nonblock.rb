# read_nonblock and the two wait classes, without a network.
#
# The shape being pinned is CRuby's retry idiom:
#
#   begin
#     data = ssl.read_nonblock(4096)
#   rescue IO::WaitReadable
#     IO.select([ssl]); retry
#   rescue IO::WaitWritable
#     IO.select(nil, [ssl]); retry
#   end
#
# Two directions, because a TLS read can need the socket to become WRITABLE
# before it can read -- a renegotiating peer does that -- so the caller has to
# wait on the right one. CRuby raises SSLErrorWaitReadable/Writable, which are
# classes including IO::WaitReadable/Writable rather than an SSLError extended
# at run time, and that is what this reproduces.
#
# Everything above the Buffered class runs under CRuby and matches it line for
# line. Below it CRuby stops, for the seam openssl_buffering.rb documents: its
# Buffering reads `@io.sync` inside #initialize, which needs the module ahead
# of the class in the ancestry.
require "openssl"

# The classes are the ones CRuby has, and they catch as both.
p OpenSSL::SSL::SSLErrorWaitReadable.new("x").is_a?(IO::WaitReadable)
p OpenSSL::SSL::SSLErrorWaitWritable.new("x").is_a?(IO::WaitWritable)
p OpenSSL::SSL::SSLErrorWaitReadable.new("x").is_a?(IO::WaitWritable)
p OpenSSL::SSL::SSLErrorWaitReadable.new("x").is_a?(OpenSSL::SSL::SSLError)
p OpenSSL::SSL::SSLErrorWaitReadable.new("x").is_a?(OpenSSL::OpenSSLError)

begin
  raise OpenSSL::SSL::SSLErrorWaitReadable, "would block"
rescue IO::WaitReadable => e
  puts "rescued as IO::WaitReadable: #{e.class}"
end

begin
  raise OpenSSL::SSL::SSLErrorWaitWritable, "would block"
rescue IO::WaitReadable
  puts "WRONG arm"
rescue IO::WaitWritable => e
  puts "rescued as IO::WaitWritable: #{e.class}"
end

# Buffering#read_nonblock answers what is buffered before it touches the
# socket, which is the case a caller waiting on the descriptor alone sleeps
# through. A stand-in stream shows that half without a handshake.
class Buffered
  include OpenSSL::Buffering

  def initialize(src)
    super()
    @src = src
    @pos = 0
  end

  def sysread(maxlen)
    return nil if @pos >= @src.bytesize
    chunk = @src[@pos, maxlen]
    @pos += chunk.bytesize
    chunk
  end

  def sysread_nonblock(maxlen, exception: true)
    raise IOError, "should not be reached while the buffer has bytes"
  end

  def syswrite_nonblock(data, exception: true)
    data.to_s.bytesize
  end

  def syswrite(data) = data.bytesize
  def sysclose = nil
end

b = Buffered.new("abcdefgh")
b.read(2)                 # fills the buffer with all 8, consumes 2
p b.read_nonblock(3)      # served from the buffer, never reaches sysread_nonblock
p b.read_nonblock(0)

# write_nonblock mirrors it: flush what is buffered, then hand the argument
# straight to the socket, so a partial write stays the caller's to retry and
# no buffer standing behind it hides how much actually went.
w = Buffered.new("")
p w.write_nonblock("hello")
p w.write_nonblock("")
