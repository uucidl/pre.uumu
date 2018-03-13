// @lang: HLSL
// orthographic vertex shader

struct Input
{
        float4 position : POSITION;
        float4 color : COLOR;
};

struct Output
{
        float4 position : SV_POSITION;
        float4 color : COLOR;
};

Output main(Input input)
{
        Output output;
        output.position = input.position;
        output.position.z = 0;
        output.position.w = 1;
        output.color = input.color;
        return output;
}
