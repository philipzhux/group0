# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected ([<<'EOF']);
(fd-reuse) begin
fd-reuse-child: exit(2)
(fd-reuse) end
fd-reuse: exit(0)
EOF
pass;
