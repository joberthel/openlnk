# analysis/ghidra_scripts/setup_dos_driver.py
# Ghidra 12.x Post-Script: Define DOS_DeviceHeader struct and annotate COM1 UART ports.
# Runs inside Ghidra headless or GUI context (PyGhidra / Jython).

try:
    from ghidra.program.model.data import (
        ArrayDataType,
        CharDataType,
        DataTypeConflictHandler,
        StructureDataType,
        UnsignedIntegerDataType,
        UnsignedShortDataType,
    )
    IN_GHIDRA = True
except ImportError:
    IN_GHIDRA = False


COM1_UART_PORTS = {
    0x3F8: "UART_COM1_THR_RBR_DLL",
    0x3F9: "UART_COM1_IER_DLM",
    0x3FA: "UART_COM1_IIR_FCR",
    0x3FB: "UART_COM1_LCR",
    0x3FC: "UART_COM1_MCR",
    0x3FD: "UART_COM1_LSR",
    0x3FE: "UART_COM1_MSR",
}


def create_dos_device_header_datatype(dtm):
    """Create the 18-byte MS-DOS Device Driver Header struct in Ghidra DataTypeManager."""
    struct_dt = StructureDataType("DOS_DeviceHeader", 0)
    struct_dt.add(UnsignedIntegerDataType.dataType, "next_driver", "Far pointer to next driver (0xFFFFFFFF for last)")
    struct_dt.add(UnsignedShortDataType.dataType, "attributes", "Bit 15: Char/Block, Bit 14: IOCTL")
    struct_dt.add(UnsignedShortDataType.dataType, "strategy_ptr", "16-bit near offset to Strategy routine")
    struct_dt.add(UnsignedShortDataType.dataType, "interrupt_ptr", "16-bit near offset to Interrupt routine")
    name_arr = ArrayDataType(CharDataType.dataType, 8, 1)
    struct_dt.add(name_arr, "name", "8-byte device name or block unit count + padding")
    return dtm.addDataType(struct_dt, DataTypeConflictHandler.REPLACE_HANDLER)


def annotate_uart_port_references(program, listing):
    """Scan instructions for immediate references to COM1 UART I/O ports (0x3F8..0x3FE)."""
    bookmarks = program.getBookmarkManager()
    inst_iter = listing.getInstructions(True)
    hits = 0
    while inst_iter.hasNext():
        inst = inst_iter.next()
        mnemonic = inst.getMnemonicString().upper()
        if mnemonic not in ("MOV", "IN", "OUT"):
            continue
        for op_idx in range(inst.getNumOperands()):
            for obj in inst.getOpObjects(op_idx):
                if hasattr(obj, "getUnsignedValue"):
                    val = int(obj.getUnsignedValue())
                    if val in COM1_UART_PORTS:
                        label = COM1_UART_PORTS[val]
                        bookmarks.setBookmark(
                            inst.getAddress(),
                            "Analysis",
                            "UART_COM1_IO",
                            f"{label} (0x{val:03X}) via {inst}",
                        )
                        hits += 1
    return hits


def run():
    if not IN_GHIDRA:
        print("[setup_dos_driver] Run this script inside Ghidra / analyzeHeadless.")
        return

    program = currentProgram  # noqa: F821 (provided by Ghidra runtime)
    dtm = program.getDataTypeManager()
    listing = program.getListing()

    header_dt = create_dos_device_header_datatype(dtm)
    print(f"[setup_dos_driver] Registered struct {header_dt.getName()} ({header_dt.getLength()} bytes)")

    uart_hits = annotate_uart_port_references(program, listing)
    print(f"[setup_dos_driver] Bookmarked {uart_hits} COM1 UART port reference(s)")


if __name__ == "__main__":
    run()
