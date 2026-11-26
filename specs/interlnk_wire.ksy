meta:
  id: interlnk_wire
  title: MS-DOS INTERLNK/INTERSVR Serial Frame Protocol
  endian: le
doc: |
  Formal specification of the INTERLNK packet format reconstructed
  via clean-room reverse engineering.

  Status: Provisional bootstrap specification. Checksum covers bytes from
  frame_type through the end of payload using CRC-16/CCITT-FALSE
  (poly 0x1021, init 0xFFFF) stored as little-endian u2 until locked
  against Golden Traces.

seq:
  - id: sync_bytes
    contents: [0x55, 0xAA]
  - id: frame_type
    type: u1
    enum: frame_types
  - id: sequence_number
    type: u1
  - id: payload_length
    type: u2
  - id: payload
    size: payload_length
    type:
      switch-on: frame_type
      cases:
        'frame_types::cmd_init': payload_init
        'frame_types::cmd_read_sector': payload_read
        'frame_types::cmd_write_sector': payload_write
        'frame_types::resp_init_ack': payload_init_ack
        'frame_types::resp_read_data': payload_read_resp
        'frame_types::resp_error': payload_error
  - id: checksum
    type: u2

enums:
  frame_types:
    0x01: cmd_init
    0x02: cmd_read_sector
    0x03: cmd_write_sector
    0x81: resp_init_ack
    0x82: resp_read_data
    0xFF: resp_error

types:
  payload_init:
    seq:
      - id: client_version
        type: u2
      - id: drive_count
        type: u1

  payload_read:
    seq:
      - id: drive_index
        type: u1
      - id: start_sector
        type: u4
      - id: sector_count
        type: u2

  payload_write:
    seq:
      - id: drive_index
        type: u1
      - id: start_sector
        type: u4
      - id: sector_count
        type: u2
      - id: data
        size: sector_count * 512

  payload_init_ack:
    seq:
      - id: server_version
        type: u2
      - id: drive_count
        type: u1
      - id: status
        type: u1

  payload_read_resp:
    seq:
      - id: drive_index
        type: u1
      - id: start_sector
        type: u4
      - id: data
        size-eos: true

  payload_error:
    seq:
      - id: error_code
        type: u1
      - id: detail
        type: u2
