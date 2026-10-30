// =============================================================================
//  HlslShim.h -- Quel tanto di HLSL che serve a compilare uno shader come C++.
//
//  Per i test di Weather/CloudShader.inl: lo stesso testo che la factory mette
//  nel materiale, qui compilato come corpo di una funzione C++. Tipi
//  (float2/3/4, uint), le funzioni intrinseche usate (floor, lerp, saturate,
//  length) e la lettura di una texture (bilineare, bordi bloccati, come il
//  campionatore della mappa meteo).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace HlslShim
{
	typedef uint32_t uint;

	struct float2
	{
		float x = 0.0f, y = 0.0f;
		float2() = default;
		float2(double X, double Y) : x(static_cast<float>(X)), y(static_cast<float>(Y)) {}
	};

	struct float3
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
		float3() = default;
		float3(double X, double Y, double Z) : x(static_cast<float>(X)), y(static_cast<float>(Y)), z(static_cast<float>(Z)) {}
	};

	struct float4
	{
		float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
		float4() = default;
		float4(double X, double Y, double Z, double W)
			: x(static_cast<float>(X)), y(static_cast<float>(Y)), z(static_cast<float>(Z)), w(static_cast<float>(W)) {}
	};

	inline float3 operator+(const float3& A, const float3& B) { return float3(A.x + B.x, A.y + B.y, A.z + B.z); }
	inline float3 operator-(const float3& A, const float3& B) { return float3(A.x - B.x, A.y - B.y, A.z - B.z); }
	inline float3 operator*(const float3& A, const float3& B) { return float3(A.x * B.x, A.y * B.y, A.z * B.z); }
	inline float3 operator*(const float3& A, float B) { return float3(A.x * B, A.y * B, A.z * B); }
	inline float3 operator/(const float3& A, const float3& B) { return float3(A.x / B.x, A.y / B.y, A.z / B.z); }

	inline float3 floor(const float3& A) { return float3(std::floor(A.x), std::floor(A.y), std::floor(A.z)); }
	inline float lerp(float A, float B, float T) { return A + (B - A) * T; }
	inline float saturate(float A) { return std::min(1.0f, std::max(0.0f, A)); }
	inline float max(float A, float B) { return std::max(A, B); }
	inline float length(const float3& A) { return std::sqrt(A.x * A.x + A.y * A.y + A.z * A.z); }

	/** Una texture BGRA8 quadrata, come la mappa meteo. */
	struct FTexture
	{
		int Size = 0;
		const std::vector<uint8_t>* Bgra = nullptr;
	};
	struct FSampler {};

	/** Bilineare, bordi bloccati, mip 0: quello che fa il campionatore della mappa. Ritorna rgba in 0..1. */
	inline float4 Texture2DSampleLevel(const FTexture& Texture, const FSampler&, const float2& Uv, float)
	{
		if (!Texture.Bgra || Texture.Size <= 0) { return float4(0.0, 0.0, 0.0, 0.0); }
		const double X = Uv.x * Texture.Size - 0.5, Y = Uv.y * Texture.Size - 0.5;
		const int X0 = static_cast<int>(std::floor(X)), Y0 = static_cast<int>(std::floor(Y));
		const double Fx = X - X0, Fy = Y - Y0;
		auto Texel = [&Texture](int Column, int Row, int Channel)
		{
			Column = std::min(Texture.Size - 1, std::max(0, Column));
			Row = std::min(Texture.Size - 1, std::max(0, Row));
			return (*Texture.Bgra)[(static_cast<size_t>(Row) * Texture.Size + Column) * 4 + Channel] / 255.0;
		};
		auto Bilinear = [&](int Channel)
		{
			const double Bottom = Texel(X0, Y0, Channel) * (1.0 - Fx) + Texel(X0 + 1, Y0, Channel) * Fx;
			const double Top = Texel(X0, Y0 + 1, Channel) * (1.0 - Fx) + Texel(X0 + 1, Y0 + 1, Channel) * Fx;
			return Bottom * (1.0 - Fy) + Top * Fy;
		};
		// BGRA in memoria: r = byte 2, g = byte 1, b = byte 0.
		return float4(Bilinear(2), Bilinear(1), Bilinear(0), Bilinear(3));
	}
}
