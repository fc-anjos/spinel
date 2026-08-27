# OpenSSL::Buffering built on #sysread / #syswrite, without a network.
#
# The module is what makes an SSLSocket read like an IO, so the contract worth
# pinning is IO's: #read with a size answers nil at EOF and #read without one
# answers "", #gets keeps its separator unless chomp, #readpartial returns as
# soon as ANY bytes are there.
#
# The oracle for the reader half is CRuby's own IO. This file cannot be run
# under CRuby directly -- its Buffering reads `@io.sync` from inside
# #initialize, which needs the module ahead of the class in the ancestry, and
# an included module is behind it -- so the expectations were taken by running
# the identical sequence against a CRuby StringIO. All sixteen agree.
require "openssl"

# A stand-in for the TLS layer: the two primitives Buffering is built on, with
# a scripted stream behind them.
class FakeStream
  include OpenSSL::Buffering
  attr_reader :written

  def initialize(src)
    super()
    @src = src
    @pos = 0
    @written = String.new
  end

  def sysread(maxlen)
    return nil if @pos >= @src.bytesize
    chunk = @src[@pos, maxlen]
    @pos += chunk.bytesize
    chunk
  end

  def syswrite(data)
    @written << data
    data.bytesize
  end

  def sysclose
    nil
  end
end

s = FakeStream.new("alpha\nbeta\ngamma\n")
p s.gets
p s.gets(chomp: true)
p s.read(3)
p s.read
# Past EOF: a sized read answers nil, an unsized one "".
p s.read(4)
p s.read
p s.eof?

# readpartial hands back what is buffered rather than waiting for the rest.
t = FakeStream.new("0123456789")
p t.readpartial(4)
p t.readpartial(100)

# Line iteration and collection.
u = FakeStream.new("a\nb\nc\n")
lines = []
u.each_line { |l| lines << l }
p lines
p FakeStream.new("x\ny\n").readlines

# Bytes and characters.
v = FakeStream.new("AB")
p v.getbyte
p v.getc
p v.getbyte

# gets with a separator that is not a newline, and a limit.
w = FakeStream.new("one|two|three")
p w.gets("|")
p w.gets("|", 2)

# The write side buffers and flushes through #syswrite.
x = FakeStream.new("")
x.write("a", "b")
x.print("c")
x << "d"
x.puts("e")
x.puts
x.flush
p x.written

# readline raises at EOF where gets answers nil.
y = FakeStream.new("only\n")
p y.readline
begin
  y.readline
rescue EOFError => e
  puts "EOFError: #{e.message}"
end
