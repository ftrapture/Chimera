#pragma once

#include <vector>
#include <cstring>

namespace chimera::ml {

inline std::vector<float> GetDefaultNeuralWeights() {
    std::vector<float> weights(3795, 0.0f);

    // --- Layer 1 (6 inputs -> 32 outputs, 3x3 kernel) ---
    // Total weights = 6 * 32 * 9 = 1728
    // Total biases = 32 (starts at index 1728)
    // Co represents low-res output channel (0..31)
    // Ci represents input channel (0..2: YCoCg, 3: Depth, 4..5: Motion)
    
    // We implement bilinear upsampling for Y, Co, Cg (input 0..2) and Depth (input 3) and Motion (input 4..5)
    // Low-res output channel: low_res_ch = hc * 4 + (dy * 2 + dx)
    // For each high-res channel hc (0..5):
    for (int hc = 0; hc < 6; ++hc) {
        int ci = hc; // Map input channel hc directly

        // dx, dy are sub-pixel offsets (0 or 1)
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                int co = hc * 4 + (dy * 2 + dx);

                // Set bilinear weights in the 3x3 kernel:
                // kx, ky in -1..1 (represented as 0..2)
                float kernel[3][3] = {0.0f};

                if (dy == 0 && dx == 0) {
                    // Top-Left subpixel
                    kernel[1][1] = 0.5625f; // Center
                    kernel[1][0] = 0.1875f; // Left
                    kernel[0][1] = 0.1875f; // Top
                    kernel[0][0] = 0.0625f; // Top-Left
                } else if (dy == 0 && dx == 1) {
                    // Top-Right subpixel
                    kernel[1][1] = 0.5625f; // Center
                    kernel[1][2] = 0.1875f; // Right
                    kernel[0][1] = 0.1875f; // Top
                    kernel[0][2] = 0.0625f; // Top-Right
                } else if (dy == 1 && dx == 0) {
                    // Bottom-Left subpixel
                    kernel[1][1] = 0.5625f; // Center
                    kernel[1][0] = 0.1875f; // Left
                    kernel[2][1] = 0.1875f; // Bottom
                    kernel[2][0] = 0.0625f; // Bottom-Left
                } else if (dy == 1 && dx == 1) {
                    // Bottom-Right subpixel
                    kernel[1][1] = 0.5625f; // Center
                    kernel[1][2] = 0.1875f; // Right
                    kernel[2][1] = 0.1875f; // Bottom
                    kernel[2][2] = 0.0625f; // Bottom-Right
                }

                // Write to weights array
                // Index formula: co * 6 * 9 + ((ky + 1) * 3 + (kx + 1)) * 6 + ci
                for (int ky = -1; ky <= 1; ++ky) {
                    for (int kx = -1; kx <= 1; ++kx) {
                        int w_idx = co * 6 * 9 + ((ky + 1) * 3 + (kx + 1)) * 6 + ci;
                        weights[w_idx] = kernel[ky + 1][kx + 1];
                    }
                }
            }
        }
    }

    // Set biases for Layer 1 to 0 (already 0 by default)

    // --- Layer 3 (11 inputs -> 16 outputs, 3x3 kernel) ---
    // Total weights = 11 * 16 * 9 = 1584
    // Start index = 1760
    // Total biases = 16 (starts at index 3344)
    // Co represents output channel (0..15)
    // Ci represents input channel (0..7: upsampled features, 8..10: history YCoCg)
    
    // We blend current YCoCg (upsampled features 0..2) and history YCoCg (history 8..10)
    // Blend: 0.1 * current + 0.9 * history
    for (int c = 0; c < 3; ++c) {
        int co = c;

        // Current color input (ci = c)
        // Center weight = 0.1f
        int w_current_idx = 1760 + co * 11 * 9 + (1 * 3 + 1) * 11 + c;
        weights[w_current_idx] = 0.10f;

        // History color input (ci = 8 + c)
        // Center weight = 0.90f
        int w_history_idx = 1760 + co * 11 * 9 + (1 * 3 + 1) * 11 + (8 + c);
        weights[w_history_idx] = 0.90f;
    }

    // Set biases for Layer 3 to 0

    // --- Layer 4 (16 inputs -> 3 outputs, 3x3 kernel) ---
    // Total weights = 16 * 3 * 9 = 432
    // Start index = 3360
    // Total biases = 3 (starts at index 3792)
    // Co represents output channel (0..2: YCoCg)
    // Ci represents input channel (0..15: Layer 3 outputs)
    
    // We pass through the blended YCoCg directly
    for (int c = 0; c < 3; ++c) {
        int co = c;
        int ci = c;

        // Center weight = 1.0f
        int w_idx = 3360 + co * 16 * 9 + (1 * 3 + 1) * 16 + ci;
        weights[w_idx] = 1.0f;
    }

    // Set biases for Layer 4 to 0

    return weights;
}

} // namespace chimera::ml
