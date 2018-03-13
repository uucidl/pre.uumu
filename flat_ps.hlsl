// @lang: HLSL
// Flat pixel shader

float4 main(float4 screenSpace : SV_Position, float4 color : COLOR) : SV_Target
{
        return color;
}