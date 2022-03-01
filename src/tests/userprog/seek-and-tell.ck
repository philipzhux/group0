# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_USER_FAULTS => 1, [<<'EOF']);
(seek-and-tell) begin
buff: Electronic Fact
buff: "Amazing Electr
(seek-and-tell) end
seek-and-tell: exit(0)
EOF
pass;
