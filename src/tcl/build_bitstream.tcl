set projName [lindex $argv 0]
set outputFolder [lindex $argv 1]
puts "$projName chosen"

open_project ./$projName/$projName.xpr

launch_runs synth_1
wait_on_run synth_1
open_run synth_1
report_timing_summary -file $outputFolder/$projName.timing_synth.log

launch_runs impl_1 -to_step write_bitstream
wait_on_run impl_1
open_run impl_1
report_timing_summary -file $outputFolder/$projName.timing_impl.log