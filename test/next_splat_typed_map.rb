# A splat-valued `next` contributes the array value to the map result.
p (1..3).map { |v| next *[v, v] if v == 2; v }
