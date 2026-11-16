// Shader Model 5.1, supported by the installed Broadwell driver.
StructuredBuffer<uint> Input : register(t0);
RWStructuredBuffer<uint> Output : register(u0);
cbuffer Params : register(b0) { uint ElementCount; uint Salt; };

uint Mix(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < ElementCount)
        Output[id.x] = Mix(Input[id.x] ^ Salt ^ (id.x * 0x9e3779b9u));
}

struct Vertex { float4 position : SV_Position; float3 color : COLOR0; };
Vertex VSMain(uint id : SV_VertexID)
{
    Vertex vertex;
    if (id == 0) { vertex.position = float4(0.0, 0.75, 0.0, 1.0); vertex.color = float3(1, 0, 0); }
    else if (id == 1) { vertex.position = float4(0.75, -0.75, 0.0, 1.0); vertex.color = float3(0, 1, 0); }
    else { vertex.position = float4(-0.75, -0.75, 0.0, 1.0); vertex.color = float3(0, 0, 1); }
    return vertex;
}
float4 PSMain(Vertex vertex) : SV_Target { return float4(vertex.color, 1.0); }
