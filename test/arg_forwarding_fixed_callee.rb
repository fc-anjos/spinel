# Forwarding preserves both channels when the callee mixes a flexible channel
# with a fixed positional or keyword parameter.
def rest_and_fixed_kw(*a, k: 0) = [a, k]
def forward_to_rest_and_fixed_kw(...) = rest_and_fixed_kw(...)
p forward_to_rest_and_fixed_kw(1, 2, k: 9)
p forward_to_rest_and_fixed_kw(3)

def fixed_pos_and_kwrest(x, **k) = [x, k]
def forward_to_fixed_pos_and_kwrest(...) = fixed_pos_and_kwrest(...)
p forward_to_fixed_pos_and_kwrest(4, extra: 8)
