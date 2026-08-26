# Empty collections return nil from min/max even when a comparator block is
# supplied. Keep nonempty and argument-taking forms covered at the same time.
class EmptyInts
  include Enumerable
  def each; end
end

plain_array = []
array = [1].first(0)
enum = EmptyInts.new

p plain_array.min { |a, b| a <=> b }
p plain_array.max { |a, b| a <=> b }
p array.min { |a, b| a <=> b }
p array.max { |a, b| a <=> b }
p enum.min { |a, b| a <=> b }
p enum.max { |a, b| a <=> b }

values = [3, 1, 2]
p values.min { |a, b| a <=> b }
p values.max { |a, b| a <=> b }
p values.min(2) { |a, b| a <=> b }
p values.max(2) { |a, b| a <=> b }
