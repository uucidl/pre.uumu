// @lang: HLSL
// Flat pixel shader

Texture2D shaderTexture;
SamplerState sampleType {
  AddressU = Clamp;
  AddressV = Clamp;
  Filter = MIN_MAG_MIP_LINEAR;
};

float4 main(float4 screenSpace : SV_Position, float4 color : COLOR, float2 texcoords : TEXCOORD) : SV_Target
{
        return shaderTexture.Sample(sampleType, texcoords);
}
