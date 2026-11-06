// Shared by the SM 6.x feature shaders (tests/src/dx12_sm6x.c). Root signature: root UAV u0 (raw output),
// root SRV t0 (raw input: words 0..63 = 0..63, words 64.. = constants below), root constants b0.
RWByteAddressBuffer o : register(u0);
ByteAddressBuffer   inp : register(t0);
cbuffer K : register(b0) { uint4 k; };
