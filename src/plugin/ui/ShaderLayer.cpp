#include "plugin/ui/ShaderLayer.h"

#include <regex>

namespace concha::ui {

using namespace juce::gl;

void ShaderLayer::setSource(const std::string& source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == source_) return;
    source_ = source;
    ++version_;
}

bool ShaderLayer::active() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !source_.empty();
}

void ShaderLayer::setUniforms(Uniforms u)
{
    std::lock_guard<std::mutex> lock(mutex_);
    uniforms_ = std::move(u);
}

std::vector<std::string> ShaderLayer::takeErrors()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(errors_);
    return out;
}

void ShaderLayer::release()
{
    program_.reset();
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (texture_) glDeleteTextures(1, &texture_);
    vbo_ = texture_ = 0;
    compiled_ = 0;
}

void ShaderLayer::render(juce::OpenGLContext& gl, juce::Rectangle<int> viewport, int targetHeight)
{
    std::string source;
    uint64_t version;
    Uniforms u;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source = source_;
        version = version_;
        u = uniforms_;
    }
    if (source.empty() || viewport.isEmpty()) return;
    if (version != compiled_) {
        compiled_ = version;
        program_.reset();
        // the user's code, wrapped as Shadertoy does unless it has its own main()
        const juce::String header = "uniform vec3 iResolution;\nuniform float iTime;\nuniform float iCycle;\nuniform float iCps;\n"
                                    "uniform int iFrame;\nuniform sampler2D iChannel0;\nuniform vec2 iOffset;\n";
        const juce::String body = juce::String::fromUTF8(source.c_str());
        juce::String fragment = header + "#line 1\n" + body + "\n";
        const bool ownMain = std::regex_search(source, std::regex(R"(\bvoid\s+main\s*\()"));
        if (!ownMain) fragment += "void main() { vec4 c = vec4(0.0); mainImage(c, gl_FragCoord.xy - iOffset); gl_FragColor = c; }\n";
        auto p = std::make_unique<juce::OpenGLShaderProgram>(gl);
        const juce::String vertex = "attribute vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
        if (!p->addVertexShader(juce::OpenGLHelpers::translateVertexShaderToV3(vertex)) ||
            !p->addFragmentShader(juce::OpenGLHelpers::translateFragmentShaderToV3(fragment)) || !p->link()) {
            std::lock_guard<std::mutex> lock(mutex_);
            errors_.push_back("[shader] " + p->getLastError().trim().toStdString());
            return;
        }
        program_ = std::move(p);
        frame_ = 0;
    }
    if (!program_) return;
    if (!vbo_) {
        const GLfloat quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    }
    if (!texture_) glGenTextures(1, &texture_);
    // the audio texture: 512 x 2, the value in red
    std::vector<uint8_t> pixels(512 * 2 * 4, 0);
    for (int row = 0; row < 2; ++row) {
        const auto& src = row == 0 ? u.spectrum : u.wave;
        for (int x = 0; x < 512 && x < int(src.size()); ++x) {
            const uint8_t v = uint8_t(juce::jlimit(0.0f, 1.0f, src[size_t(x)]) * 255.0f);
            uint8_t* px = pixels.data() + (row * 512 + x) * 4;
            px[0] = px[1] = px[2] = v;
            px[3] = 255;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // GL's origin is the bottom left
    const int y = targetHeight - viewport.getBottom();
    glViewport(viewport.getX(), y, viewport.getWidth(), viewport.getHeight());
    glEnable(GL_SCISSOR_TEST);
    glScissor(viewport.getX(), y, viewport.getWidth(), viewport.getHeight());
    glDisable(GL_BLEND);
    program_->use();
    auto set1 = [&](const char* name, float v) {
        const GLint loc = glGetUniformLocation(program_->getProgramID(), name);
        if (loc >= 0) glUniform1f(loc, v);
    };
    if (GLint loc = glGetUniformLocation(program_->getProgramID(), "iResolution"); loc >= 0)
        glUniform3f(loc, float(viewport.getWidth()), float(viewport.getHeight()), 1.0f);
    if (GLint loc = glGetUniformLocation(program_->getProgramID(), "iOffset"); loc >= 0) glUniform2f(loc, float(viewport.getX()), float(y));
    if (GLint loc = glGetUniformLocation(program_->getProgramID(), "iFrame"); loc >= 0) glUniform1i(loc, frame_++);
    if (GLint loc = glGetUniformLocation(program_->getProgramID(), "iChannel0"); loc >= 0) glUniform1i(loc, 0);
    set1("iTime", float(u.time));
    set1("iCycle", float(u.cycle));
    set1("iCps", float(u.cps));
    for (const auto& [name, v] : u.haps) {
        set1(name.c_str(), v.first);
        set1((name + "_v").c_str(), v.second);
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLint pos = glGetAttribLocation(program_->getProgramID(), "position");
    if (pos >= 0) {
        glEnableVertexAttribArray(GLuint(pos));
        glVertexAttribPointer(GLuint(pos), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(GLuint(pos));
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
}

} // namespace concha::ui
