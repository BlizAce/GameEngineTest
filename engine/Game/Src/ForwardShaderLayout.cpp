#include "Hollowpoint/Game/ForwardShaderLayout.h"

namespace Hollowpoint::Game::ShaderLayout
{

float EncodeLightType(Renderer::LightComponent::LightType type) noexcept
{
    switch (type)
    {
        case Renderer::LightComponent::LightType::Directional:
            return 0.0f;
        case Renderer::LightComponent::LightType::Point:
            return 1.0f;
        case Renderer::LightComponent::LightType::Spot:
            return 2.0f;
    }

    // Unreachable for the three-value enum; returning "directional" rather than asserting keeps a corrupt component
    // from killing a frame, and matches what an uninitialised byte's most likely interpretation would be anyway.
    return 0.0f;
}

}  // namespace Hollowpoint::Game::ShaderLayout
