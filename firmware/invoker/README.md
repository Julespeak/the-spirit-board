# INVOKER Firmware

**INVOKER** — *Infernal Nibble-Vector Oracle & Kinetic Event Receiver* — is the
firmware module responsible for:

- Driving the Furby sound coprocessor with timed 4-bit nibble vectors
- Monitoring `/RTS` to measure sound completion and bus readiness
- Providing a higher-level interface for “invoking” audio phrases
  from the SPIRIT board
