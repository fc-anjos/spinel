# `return` / `next` from a rescue or else clause under `ensure` leaves the
# handler stack intact for later rescues.
def ret_from_rescue
  raise "x"
rescue => e
  return "r:#{e.message}"
ensure
  puts "ensure ran"
end

def ret_from_else
  1
rescue
  return "rescued"
else
  return "else"
ensure
  puts "ensure ran"
end

def next_from_rescue(xs)
  out = []
  xs.each do |x|
    begin
      raise "odd" if x.odd?
      out << x
    rescue
      next
    ensure
      out << -x
    end
  end
  out
end

def nested_ret
  begin
    raise "inner"
  rescue
    return "inner-rescue"
  ensure
    puts "inner ensure"
  end
ensure
  puts "outer ensure"
end

p ret_from_rescue
p ret_from_else
p next_from_rescue([1, 2, 3])
p nested_ret

# every later rescue must still catch
begin
  raise "boom"
rescue => e
  p e.message
end
p((Integer("zz") rescue -1))
x = (raise "mod" rescue "modifier")
p x
