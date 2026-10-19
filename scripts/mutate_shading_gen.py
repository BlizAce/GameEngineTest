#!/usr/bin/env python3
"""Applies one named mutation to the shading GLSL generator, verifying it landed."""
import pathlib
import sys

GEN = pathlib.Path('engine/Renderer/Src/ShadingModelGlsl.cpp')

MUTATIONS = {
    # (description, old, new)
    'ggx_pi': ('GGX denominator uses 3.0 instead of shared pi',
               'glsl += "    float denominator = " + pi + " * denominatorTerm * denominatorTerm;\\n";',
               'glsl += "    float denominator = 3.0 * denominatorTerm * denominatorTerm;\\n";'),
    'fresnel_exp': ('Fresnel exponent 5 -> 4',
                    'glsl += "               * oneMinusCosine * oneMinusCosine;\\n";',
                    'glsl += ";\\n";'),
    'dielectric_f0': ('dielectric F0 0.04 -> 0.05',
                      'glsl += "    float dielectricF0 = 0.04;\\n";',
                      'glsl += "    float dielectricF0 = 0.05;\\n";'),
    'double_cosine': ('cosine applied twice to diffuse',
                      'glsl += "    outDiffuse = diffuseReflectance * dotNormalLight;\\n";',
                      'glsl += "    outDiffuse = diffuseReflectance * dotNormalLight * dotNormalLight;\\n";'),
    'roughness_linear': ('perceptual roughness no longer squared',
                         'glsl += "    return max(clamped * clamped, " + minAlpha + ");\\n";',
                         'glsl += "    return max(clamped, " + minAlpha + ");\\n";'),
}


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in MUTATIONS:
        print(f"usage: {sys.argv[0]} <{'|'.join(MUTATIONS)}>")
        return 2

    name = sys.argv[1]
    description, old, new = MUTATIONS[name]
    text = GEN.read_text()

    if old not in text:
        print(f"ANCHOR-MISSING ({name}): the mutation was NOT applied — any test result is meaningless")
        return 3

    GEN.write_text(text.replace(old, new, 1))

    # Verify the file on disk actually changed before anything reads a test result.
    if new not in GEN.read_text():
        print(f"VERIFY-FAILED ({name}): write did not persist")
        return 4

    print(f"APPLIED ({name}): {description}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
