# Shared preflight for the HLS-generated software register ABI.
proc soc_gemv_check_registers {ip_repo} {
    set headers [glob -nocomplain "$ip_repo/drivers/*/src/*_hw.h"]
    if {[llength $headers] != 1} { error "Expected one generated HLS register header under $ip_repo/drivers" }
    set f [open [lindex $headers 0] r]
    set text [read $f]
    close $f
    foreach {suffix expected} {
        AP_CTRL 0x00 GIE 0x04 W_QUANTS_DATA 0x10 W_SCALES_DATA 0x20
        X_NATIVE_DATA 0x30 Y_DATA 0x40 ROWS_DATA 0x50 NB_DATA 0x58
        STATUS_DATA 0x60 STATUS_CTRL 0x64
    } {
        set macro XSOC_GEMV_HW_CONTROL_ADDR_$suffix
        set pattern [format {#define\s+%s\s+(0x[0-9a-fA-F]+|[0-9]+)} $macro]
        if {![regexp $pattern $text -> value] || $value != $expected} {
            error "Register ABI mismatch for $macro (expected $expected); do not use the HAL with this export"
        }
    }
    puts "Generated HLS register ABI matches HAL: [lindex $headers 0]"
}
