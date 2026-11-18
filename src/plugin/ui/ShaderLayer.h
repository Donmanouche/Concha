#pragma once

// Concha's shader(`...`): a GLSL fragment shader drawn behind the code, in the window's OpenGL
// context, before JUCE paints the components on top. Shadertoy conventions:
//   void mainImage(out vec4 fragColor, in vec2 fragCoord)
//   uniform vec3 iResolution; float iTime (s), iCycle, iCps; int iFrame; sampler2D iChannel0
//   (512 x 2: row 0 the output's spectrum, row 1 its waveform, 0..1 in .r, as Shadertoy's audio).
// Hap uniforms: a `uniform float <name>;` gets the seconds since the last onset of the haps with
// .uniform("<name>"), `<name>_v` their value (gain * velocity).

#include <juce_opengl/juce_opengl.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace concha::ui {

class ShaderLayer {
public:
    // message thread
    void setSource(const std::string& source); // empty: no shader
    bool active() const;
    struct Uniforms {
        double time = 0, cycle = 0, cps = 0.5;
        std::map<std::string, std::pair<float, float>> haps; // name -> (seconds since onset, value)
        std::vector<float> spectrum, wave;                    // 512 each, 0..1
    };
    void setUniforms(Uniforms u);
    std::vector<std::string> takeErrors(); // compile errors, for the console

    // OpenGL thread
    void render(juce::OpenGLContext& gl, juce::Rectangle<int> viewport, int targetHeight);
    void release(); // the context closes

private:
    mutable std::mutex mutex_;
    std::string source_;
    uint64_t version_ = 0, compiled_ = 0;
    Uniforms uniforms_;
    std::vector<std::string> errors_;
    int frame_ = 0;

    std::unique_ptr<juce::OpenGLShaderProgram> program_;
    GLuint vbo_ = 0, texture_ = 0;
};

} // namespace concha::ui
