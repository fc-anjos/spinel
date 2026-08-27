# A Time that arrives boxed -- read out of a container, or narrowed out of an
# untyped value by is_a?(Time) -- reached a poly surface that had only the
# scalar accessors on it. Everything that answers a Time, a boolean, an offset,
# or a formatted string raised NoMethodError at run time, with nothing wrong in
# the generated C to warn about it. (matz/spinel#4109)
#
# The array is a poly container, so every receiver below is boxed. The instant
# is put in UTC first: zone, iso8601 and the local reads would otherwise answer
# whatever zone the machine running the suite happens to be in.
box = [Time.at(1700000000, 123456).utc, "not a time"]
t = box[0]

# The ones that answer a Time, which is what the report named. The result stays
# boxed, so a chained read dispatches through this same surface. These are the
# non-mutating spellings; the converting ones come last, since in Ruby they
# change the receiver.
puts t.getutc.year
puts t.getgm.year
puts t.getlocal(0).hour
puts t.getlocal("+00:00").hour

# The offsets and the subsecond reads.
puts t.nsec
puts t.tv_nsec
puts t.usec
puts t.tv_usec
puts t.utc_offset
puts t.gmt_offset
puts t.gmtoff
puts t.subsec

# The predicates.
puts t.utc?
puts t.gmt?
puts t.dst?
puts t.isdst
puts t.sunday?
puts t.monday?
puts t.tuesday?
puts t.wednesday?
puts t.thursday?
puts t.friday?
puts t.saturday?

# The formatters.
puts t.zone
puts t.iso8601
puts t.xmlschema

# localtime / getlocal answer the machine's zone, so only the year is stable
# enough to assert -- this instant is mid-November in every zone on earth.
puts t.getlocal.year

# The scalar half that already worked, so a change here would be visible.
puts t.year
puts t.hour
puts t.to_i

# A receiver that is not a Time still raises, and the message names the method.
begin
  box[1].utc
rescue NoMethodError => e
  puts "raised: #{e.message.include?("utc")}"
end

# utc / gmtime / localtime convert the receiver in place and answer it, so they
# come last: the reads above would see the new zone. The other reference to the
# same Time sees the conversion too, as it does in Ruby.
also = t
puts t.localtime.year
puts t.utc.year
puts t.utc?
puts also.utc?
puts t.gmtime.hour
