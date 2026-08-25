h = {}
h[57] = -4

h.keys.sort.each do |key|
  value = h[key]
  puts(key.to_s + "=" + (value.is_a?(Array) ?
    ("[" + value.map { |item| item.to_s }.join(",") + "]") :
    (value.is_a?(Hash) ?
      ("{" + value.keys.sort_by(&:to_s).map { |nested_key| nested_key.to_s + ":" + value[nested_key].to_s }.join(",") + "}") :
      (value.is_a?(Float) ? format("%.2f", value) : value.to_s))))
end

# A bool's class depends on its value, and a module target is an ancestor:
# neither guard may be folded away.
flag = true
puts(flag.is_a?(TrueClass) ? "true-class" : "false-class")
n = 5
puts(n.is_a?(Comparable) ? "comparable" : "not")
puts(n.is_a?(Numeric) ? "numeric" : "not")
puts(n.is_a?(String) ? "string" : "not-string")
s = "x"
puts(s.instance_of?(String) ? "exact" : "inexact")
puts(s.is_a?(Object) ? "object" : "not")
