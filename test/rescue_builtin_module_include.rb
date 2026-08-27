# `include IO::WaitReadable` -- a BUILTIN module named through its path -- was
# dropped on the floor: the include recorder looked the name up in the class
# table, found nothing (a builtin module has no user class), and skipped it
# without a word. So `rescue IO::WaitReadable` fell through to the next arm
# and `e.is_a?(IO::WaitReadable)` answered false, for an exception class that
# said it included it.
#
# This is how a non-blocking read reports "would block" in CRuby:
# OpenSSL::SSL::SSLErrorWaitReadable is a class including IO::WaitReadable,
# not an SSLError extended at run time.
module Plain
end

class ErrPlain < StandardError
  include Plain
end

class ErrWaitR < StandardError
  include IO::WaitReadable
end

class ErrWaitW < StandardError
  include IO::WaitWritable
end

# A user module already worked; the builtin one is what did not.
begin
  raise ErrPlain, "a"
rescue Plain => e
  puts "user module: #{e.class}"
end

begin
  raise ErrWaitR, "b"
rescue IO::WaitReadable => e
  puts "builtin module: #{e.class}"
rescue StandardError
  puts "WRONG: fell through"
end

# The two directions do not alias each other.
begin
  raise ErrWaitW, "c"
rescue IO::WaitReadable
  puts "WRONG arm"
rescue IO::WaitWritable => e
  puts "other direction: #{e.class}"
end

p ErrWaitR.new("x").is_a?(IO::WaitReadable)
p ErrWaitR.new("x").is_a?(IO::WaitWritable)
p ErrWaitR.new("x").is_a?(StandardError)

# An inherited include reaches the subclass too.
class ErrChild < ErrWaitR
end
begin
  raise ErrChild, "d"
rescue IO::WaitReadable => e
  puts "inherited: #{e.class}"
end
