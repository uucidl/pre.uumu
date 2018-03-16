// @lang: HLSL
// orthographic vertex shader

struct Input
{
        float4 position : POSITION;
        float4 color : COLOR;
        float2 texcoord : TEXCOORD;
};

struct Output
{
        float4 position : SV_POSITION;
        float4 color : COLOR;
        float2 texcoord : TEXCOORD;
};

Output main(Input input)
{
        Output output;
        output.position = input.position;
        output.position.z = 0;
        output.position.w = 1;
        output.color = input.color;
        output.texcoord = input.texcoord;
        return output;
}
