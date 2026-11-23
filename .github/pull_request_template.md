## Summary
<!-- Briefly describe what this PR changes and why. -->

## Checklist
- [ ] Built cleanly with `make -j$(nproc) CFLAGS="-std=c17 -Wall -Wextra -Wpedantic -Werror -MMD -MP"`
- [ ] All unit/integration tests pass (`make test`)
- [ ] Sanitizer suite passes without leaks or UB (`make test-debug`)
- [ ] Static analysis passes without new warnings (`make static-analysis`)
- [ ] Updated `po/POTFILES.in` if new translatable `.c` or `.ui` files were added
