// BalatroPhone.fx
// Composites Balatro onto GTA V's in-game phone screen and handles fullscreen mode.

#include "ReShade.fxh"

texture BalatroTex : BALATRO_TEX;
sampler sBalatro { Texture = BalatroTex; AddressU = CLAMP; AddressV = CLAMP; };

uniform bool BalatroActive = false;
uniform int DisplayMode = 0; // 0 = Standard Phone, 1 = Large Phone, 2 = Fullscreen
uniform int CamMode = 0;     // 4 = First-person

// Default coordinates matching GTA V's landscape iFruit phone screen
uniform float PhoneX1 = 0.7000;
uniform float PhoneY1 = 0.5470;
uniform float PhoneX2 = 0.9160;
uniform float PhoneY2 = 0.7760;
uniform float MouseX = 0.5;
uniform float MouseY = 0.5;

float4 PS_BalatroPhone(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 gtaColor = tex2D(ReShade::BackBuffer, uv);
    if (!BalatroActive)
        return gtaColor;

    // Mode 0 (Standard Phone / First-Person):
    // Rendered directly onto GTA V's in-engine phone screen via PHONE_SCREEN render target.
    // Zero 2D overlay to avoid floating duplicate screens.
    if (DisplayMode == 0)
        return gtaColor;

    // DisplayMode 1: Large Phone Mode (High-Res 1080p Screen Composite)
    if (DisplayMode == 1)
    {
        if (uv.x >= PhoneX1 && uv.x <= PhoneX2 && uv.y >= PhoneY1 && uv.y <= PhoneY2)
        {
            float2 phoneUV = float2((uv.x - PhoneX1) / (PhoneX2 - PhoneX1),
                                    (uv.y - PhoneY1) / (PhoneY2 - PhoneY1));
            float3 col = tex2D(sBalatro, phoneUV).rgb;
            // Gamma correction (1.20) matching fullscreen mode
            gtaColor = float4(pow(col, 1.20), 1.0);
        }
    }
    // DisplayMode 2: Fullscreen Mode
    else if (DisplayMode == 2)
    {
        float3 col = tex2D(sBalatro, uv).rgb;
        // Apply 1.20 gamma correction
        gtaColor = float4(pow(col, 1.20), 1.0);
    }

    return gtaColor;
}

technique BalatroPhone
{
    pass Composite
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_BalatroPhone;
    }
}
