// SM 6.6 dynamic resources: ResourceDescriptorHeap / SamplerDescriptorHeap.
[numthreads(1, 1, 1)]
void main() {
    RWByteAddressBuffer dst = ResourceDescriptorHeap[0];
    ByteAddressBuffer src = ResourceDescriptorHeap[1];
    Texture2D<float4> tex = ResourceDescriptorHeap[2];
    SamplerState smp = SamplerDescriptorHeap[0];
    float4 t = tex.SampleLevel(smp, float2(0.75, 0.25), 0);
    dst.Store(0, src.Load(4 * 3) + 100);
    dst.Store(4, (uint)(t.x * 255.0 + 0.5));
}
