#ifdef GEODE_IS_IOS

#include "../Shader.hpp"
#include <vector>

using namespace geode::prelude;

Shader::~Shader() {
    if (program)
        glDeleteProgram(program);
}

static void printShaderLog(u32 object, bool program) {
    char logBuffer[2048] = {};
    if (program)
        glGetProgramInfoLog(object, sizeof(logBuffer), nullptr, logBuffer);
    else
        glGetShaderInfoLog(object, sizeof(logBuffer), nullptr, logBuffer);
    if (logBuffer[0])
        log::error("{}", logBuffer);
}

static u32 compileShader(GLenum type, const std::string& source) {
    u32 shader = glCreateShader(type);
    const char* ptr = source.c_str();
    glShaderSource(shader, 1, &ptr, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        log::error("Failed to compile Bismuth iOS {} shader", type == GL_VERTEX_SHADER ? "vertex" : "fragment");
        printShaderLog(shader, false);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

Shader* Shader::create(const ShaderSources& sources) {
    u32 vertex = compileShader(GL_VERTEX_SHADER, sources.vertexSource);
    u32 fragment = compileShader(GL_FRAGMENT_SHADER, sources.fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return nullptr;
    }

    u32 linkedProgram = glCreateProgram();
    glAttachShader(linkedProgram, vertex);
    glAttachShader(linkedProgram, fragment);

    // ES2 has no layout(location = N) syntax, so pin Bismuth's shared
    // ObjectBatch attribute locations before linking.
    glBindAttribLocation(linkedProgram, 0, "a_positionOffset");
    glBindAttribLocation(linkedProgram, 1, "a_texCoord");
    glBindAttribLocation(linkedProgram, 2, "a_srbIndex");
    glBindAttribLocation(linkedProgram, 3, "a_colorChannel");

    glLinkProgram(linkedProgram);

    GLint success = GL_FALSE;
    glGetProgramiv(linkedProgram, GL_LINK_STATUS, &success);
    if (success != GL_TRUE) {
        log::error("Failed to link Bismuth iOS shader program");
        printShaderLog(linkedProgram, true);
        glDeleteProgram(linkedProgram);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return nullptr;
    }

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    auto shader = new Shader();
    shader->program = linkedProgram;
    return shader;
}

Shader* Shader::create(
    const fs::path& vertexPath,
    const fs::path& fragmentPath,
    std::map<std::string, std::string>
) {
    auto vertex = readResourceFile(vertexPath);
    auto fragment = readResourceFile(fragmentPath);
    if (!vertex || !fragment) {
        log::error("Failed to read iOS shader resources {} / {}", vertexPath.string(), fragmentPath.string());
        return nullptr;
    }
    return create({ vertex.value(), fragment.value() });
}

void Shader::setMatrix4(u32 location, const float* data) {
    use();
    glUniformMatrix4fv((GLint)location, 1, GL_FALSE, data);
}

void Shader::setInt(u32 location, i32 value) {
    use();
    glUniform1i((GLint)location, value);
}

void Shader::setUInt(u32 location, u32 value) {
    // iOS shaders represent integer-ish metadata as exact floats.
    use();
    glUniform1f((GLint)location, (float)value);
}

void Shader::setFloat(u32 location, float value) {
    use();
    glUniform1f((GLint)location, value);
}

void Shader::setVec2(u32 location, glm::vec2 value) {
    use();
    glUniform2f((GLint)location, value.x, value.y);
}

void Shader::setVec3(u32 location, glm::vec3 value) {
    use();
    glUniform3f((GLint)location, value.x, value.y, value.z);
}

void Shader::setVec4(u32 location, glm::vec4 value) {
    use();
    glUniform4f((GLint)location, value.x, value.y, value.z, value.w);
}

void Shader::setTexture(u32 location, i32 unit, u32 texture) {
    use();
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i((GLint)location, unit);
}

void Shader::setTextureArray(u32 location, i32 count, u32* textures) {
    use();
    std::vector<i32> units(count);
    for (i32 i = 0; i < count; ++i) {
        units[i] = i;
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i]);
    }
    glUniform1iv((GLint)location, count, units.data());
}

void Shader::setTextureArray(u32 location, i32 count, cocos2d::CCTexture2D** textures) {
    use();
    std::vector<i32> units(count);
    for (i32 i = 0; i < count; ++i) {
        units[i] = i;
        glActiveTexture(GL_TEXTURE0 + i);
        if (textures[i])
            glBindTexture(GL_TEXTURE_2D, textures[i]->getName());
    }
    glUniform1iv((GLint)location, count, units.data());
}

#endif
