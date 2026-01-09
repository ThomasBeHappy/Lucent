#include "lucent/material/MaterialGraphEval.h"
#include "lucent/core/Log.h"

#include <variant>
#include <unordered_map>
#include <sstream>
#include <cctype>
#include <cmath>
#include <algorithm>

namespace lucent::material {

namespace {

using Value = std::variant<float, glm::vec2, glm::vec3, glm::vec4>;

static int Components(const Value& v) {
    if (std::holds_alternative<float>(v)) return 1;
    if (std::holds_alternative<glm::vec2>(v)) return 2;
    if (std::holds_alternative<glm::vec3>(v)) return 3;
    return 4;
}

static glm::vec4 ToVec4(const Value& v) {
    if (auto* f = std::get_if<float>(&v)) return glm::vec4(*f, 0, 0, 0);
    if (auto* a = std::get_if<glm::vec2>(&v)) return glm::vec4(a->x, a->y, 0, 0);
    if (auto* a = std::get_if<glm::vec3>(&v)) return glm::vec4(a->x, a->y, a->z, 0);
    return std::get<glm::vec4>(v);
}

static Value FromVecN(const glm::vec4& v, int n) {
    if (n <= 1) return v.x;
    if (n == 2) return glm::vec2(v.x, v.y);
    if (n == 3) return glm::vec3(v.x, v.y, v.z);
    return v;
}

static Value Convert(const Value& v, int toN) {
    const int fromN = Components(v);
    if (fromN == toN) return v;
    glm::vec4 a = ToVec4(v);
    if (fromN == 1 && toN > 1) {
        // broadcast
        a = glm::vec4(a.x, a.x, a.x, a.x);
    }
    // For truncation/extension, use vec4 and take first N.
    // Extend missing channels with 0 and alpha with 1 for vec4-from-vec3 if needed.
    if (fromN == 3 && toN == 4) a.w = 1.0f;
    return FromVecN(a, toN);
}

static Value BinaryOp(const Value& a, const Value& b, const char op) {
    const int n = std::max(Components(a), Components(b));
    glm::vec4 va = ToVec4(Convert(a, n));
    glm::vec4 vb = ToVec4(Convert(b, n));
    glm::vec4 r(0.0f);
    switch (op) {
        case '+': r = va + vb; break;
        case '-': r = va - vb; break;
        case '*': r = va * vb; break;
        case '/': {
            glm::vec4 denom = glm::max(vb, glm::vec4(1e-6f));
            r = va / denom;
            break;
        }
        default: break;
    }
    return FromVecN(r, n);
}

static float AsFloat(const Value& v, float fallback = 0.0f) {
    if (auto* f = std::get_if<float>(&v)) return *f;
    if (auto* a = std::get_if<glm::vec2>(&v)) return a->x;
    if (auto* a = std::get_if<glm::vec3>(&v)) return a->x;
    if (auto* a = std::get_if<glm::vec4>(&v)) return a->x;
    return fallback;
}

static glm::vec3 AsVec3(const Value& v, glm::vec3 fallback = glm::vec3(0.0f)) {
    if (auto* a = std::get_if<glm::vec3>(&v)) return *a;
    if (auto* a = std::get_if<float>(&v)) return glm::vec3(*a);
    if (auto* a = std::get_if<glm::vec2>(&v)) return glm::vec3(a->x, a->y, 0.0f);
    if (auto* a = std::get_if<glm::vec4>(&v)) return glm::vec3(a->x, a->y, a->z);
    return fallback;
}

static bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// -----------------------
// Tiny expression parser (for CustomCode Out = <expr>;)
// Supports: literals (float), vec2/vec3/vec4 constructors, + - * /, parentheses,
// identifiers (variables), functions: sin, cos, abs, min, max, clamp, mix, pow, sqrt.
// -----------------------
enum class TokKind { End, Ident, Number, LParen, RParen, Comma, Op };
struct Token { TokKind kind; std::string text; };

struct Lexer {
    const std::string& s;
    size_t i = 0;
    explicit Lexer(const std::string& in) : s(in) {}

    void SkipWs() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    }

    Token Next() {
        SkipWs();
        if (i >= s.size()) return { TokKind::End, "" };
        char c = s[i];
        if (c == '(') { ++i; return { TokKind::LParen, "(" }; }
        if (c == ')') { ++i; return { TokKind::RParen, ")" }; }
        if (c == ',') { ++i; return { TokKind::Comma, "," }; }
        if (c == '+' || c == '-' || c == '*' || c == '/') { ++i; return { TokKind::Op, std::string(1, c) }; }
        if (std::isdigit((unsigned char)c) || c == '.') {
            size_t start = i;
            ++i;
            while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.' || s[i] == 'f')) ++i;
            return { TokKind::Number, s.substr(start, i - start) };
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            ++i;
            while (i < s.size() && IsIdentChar(s[i])) ++i;
            return { TokKind::Ident, s.substr(start, i - start) };
        }
        // Unknown: skip
        ++i;
        return Next();
    }
};

struct Parser {
    Lexer lex;
    Token cur;
    const std::unordered_map<std::string, Value>& vars;
    std::string* err = nullptr;

    Parser(const std::string& expr, const std::unordered_map<std::string, Value>& v, std::string* e)
        : lex(expr), vars(v), err(e) { cur = lex.Next(); }

    void Fail(const std::string& m) {
        if (err && err->empty()) *err = m;
    }

    bool Eat(TokKind k) {
        if (cur.kind == k) { cur = lex.Next(); return true; }
        return false;
    }

    Value ParseExpr() { return ParseAddSub(); }

    Value ParseAddSub() {
        Value v = ParseMulDiv();
        while (cur.kind == TokKind::Op && (cur.text == "+" || cur.text == "-")) {
            char op = cur.text[0];
            cur = lex.Next();
            Value rhs = ParseMulDiv();
            v = BinaryOp(v, rhs, op);
        }
        return v;
    }

    Value ParseMulDiv() {
        Value v = ParseUnary();
        while (cur.kind == TokKind::Op && (cur.text == "*" || cur.text == "/")) {
            char op = cur.text[0];
            cur = lex.Next();
            Value rhs = ParseUnary();
            v = BinaryOp(v, rhs, op);
        }
        return v;
    }

    Value ParseUnary() {
        if (cur.kind == TokKind::Op && cur.text == "-") {
            cur = lex.Next();
            Value v = ParseUnary();
            return BinaryOp(Value(0.0f), v, '-');
        }
        return ParsePrimary();
    }

    Value ParseCallOrIdent(const std::string& name) {
        if (!Eat(TokKind::LParen)) {
            auto it = vars.find(name);
            if (it != vars.end()) return it->second;
            Fail("Unknown identifier: " + name);
            return Value(0.0f);
        }

        std::vector<Value> args;
        if (cur.kind != TokKind::RParen && cur.kind != TokKind::End) {
            args.push_back(ParseExpr());
            while (Eat(TokKind::Comma)) {
                args.push_back(ParseExpr());
            }
        }
        if (!Eat(TokKind::RParen)) Fail("Expected ')'");

        auto fn1 = [&](auto f) -> Value {
            if (args.size() < 1) return Value(0.0f);
            const int n = Components(args[0]);
            glm::vec4 a = ToVec4(Convert(args[0], n));
            glm::vec4 r(0.0f);
            r.x = f(a.x); r.y = f(a.y); r.z = f(a.z); r.w = f(a.w);
            return FromVecN(r, n);
        };

        if (name == "vec2" && args.size() >= 1) {
            float x = AsFloat(args[0]);
            float y = (args.size() >= 2) ? AsFloat(args[1]) : x;
            return glm::vec2(x, y);
        }
        if (name == "vec3" && args.size() >= 1) {
            float x = AsFloat(args[0]);
            float y = (args.size() >= 2) ? AsFloat(args[1]) : x;
            float z = (args.size() >= 3) ? AsFloat(args[2]) : x;
            return glm::vec3(x, y, z);
        }
        if (name == "vec4" && args.size() >= 1) {
            float x = AsFloat(args[0]);
            float y = (args.size() >= 2) ? AsFloat(args[1]) : x;
            float z = (args.size() >= 3) ? AsFloat(args[2]) : x;
            float w = (args.size() >= 4) ? AsFloat(args[3]) : 1.0f;
            return glm::vec4(x, y, z, w);
        }

        if (name == "sin") return fn1([](float x) { return std::sin(x); });
        if (name == "cos") return fn1([](float x) { return std::cos(x); });
        if (name == "abs") return fn1([](float x) { return std::fabs(x); });
        if (name == "sqrt") return fn1([](float x) { return std::sqrt(std::max(x, 0.0f)); });

        if ((name == "min" || name == "max") && args.size() >= 2) {
            const int n = std::max(Components(args[0]), Components(args[1]));
            glm::vec4 a = ToVec4(Convert(args[0], n));
            glm::vec4 b = ToVec4(Convert(args[1], n));
            glm::vec4 r(0.0f);
            if (name == "min") r = glm::min(a, b);
            else r = glm::max(a, b);
            return FromVecN(r, n);
        }

        if (name == "clamp" && args.size() >= 3) {
            const int n = std::max({ Components(args[0]), Components(args[1]), Components(args[2]) });
            glm::vec4 x = ToVec4(Convert(args[0], n));
            glm::vec4 lo = ToVec4(Convert(args[1], n));
            glm::vec4 hi = ToVec4(Convert(args[2], n));
            glm::vec4 r = glm::min(glm::max(x, lo), hi);
            return FromVecN(r, n);
        }

        if ((name == "mix" || name == "lerp") && args.size() >= 3) {
            const int n = std::max(Components(args[0]), Components(args[1]));
            glm::vec4 a = ToVec4(Convert(args[0], n));
            glm::vec4 b = ToVec4(Convert(args[1], n));
            float t = AsFloat(args[2]);
            glm::vec4 r = (1.0f - t) * a + t * b;
            return FromVecN(r, n);
        }

        if (name == "pow" && args.size() >= 2) {
            const int n = std::max(Components(args[0]), Components(args[1]));
            glm::vec4 a = ToVec4(Convert(args[0], n));
            glm::vec4 b = ToVec4(Convert(args[1], n));
            glm::vec4 r(0.0f);
            r.x = std::pow(a.x, b.x);
            r.y = std::pow(a.y, b.y);
            r.z = std::pow(a.z, b.z);
            r.w = std::pow(a.w, b.w);
            return FromVecN(r, n);
        }

        Fail("Unsupported function: " + name);
        return Value(0.0f);
    }

    Value ParsePrimary() {
        if (cur.kind == TokKind::Number) {
            float f = 0.0f;
            try { f = std::stof(cur.text); } catch (...) { f = 0.0f; }
            cur = lex.Next();
            return f;
        }
        if (cur.kind == TokKind::Ident) {
            std::string name = cur.text;
            cur = lex.Next();
            return ParseCallOrIdent(name);
        }
        if (Eat(TokKind::LParen)) {
            Value v = ParseExpr();
            if (!Eat(TokKind::RParen)) Fail("Expected ')'");
            return v;
        }
        Fail("Unexpected token");
        cur = lex.Next();
        return Value(0.0f);
    }
};

static bool ExtractAssignmentExpr(const std::string& code, const std::string& lhs, std::string& outExpr) {
    // Find a line like: "<lhs> = <expr>;" (very simple scan).
    std::istringstream ss(code);
    std::string line;
    while (std::getline(ss, line)) {
        // strip comments
        size_t cpos = line.find("//");
        if (cpos != std::string::npos) line = line.substr(0, cpos);
        // trim
        auto trim = [](std::string s) {
            size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
            size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
            return s.substr(a, b - a);
        };
        std::string t = trim(line);
        if (t.rfind(lhs, 0) != 0) continue;
        t = trim(t.substr(lhs.size()));
        if (t.empty() || t[0] != '=') continue;
        t = trim(t.substr(1));
        if (!t.empty() && t.back() == ';') t.pop_back();
        t = trim(t);
        if (!t.empty()) { outExpr = t; return true; }
    }
    return false;
}

struct EvalCtx {
    const MaterialGraph& g;
    std::unordered_map<PinID, Value> cache;
    std::unordered_map<PinID, bool> visiting;
    std::string* err = nullptr;
};

static Value DefaultForPinType(PinType t) {
    switch (t) {
        case PinType::Float: return 0.0f;
        case PinType::Vec2: return glm::vec2(0.0f);
        case PinType::Vec3: return glm::vec3(0.0f);
        case PinType::Vec4: return glm::vec4(0.0f);
        default: return 0.0f;
    }
}

static Value PinValueToValue(const PinValue& v, PinType typeHint) {
    if (auto* f = std::get_if<float>(&v)) return *f;
    if (auto* a = std::get_if<glm::vec2>(&v)) return *a;
    if (auto* a = std::get_if<glm::vec3>(&v)) return *a;
    if (auto* a = std::get_if<glm::vec4>(&v)) return *a;
    (void)typeHint;
    return 0.0f;
}

static Value EvalPin(EvalCtx& ctx, PinID pinId);

static Value EvalInputPin(EvalCtx& ctx, PinID pinId) {
    const MaterialPin* pin = ctx.g.GetPin(pinId);
    if (!pin) return 0.0f;
    LinkID linkId = ctx.g.FindLinkByEndPin(pinId);
    if (linkId != INVALID_LINK_ID) {
        const MaterialLink* link = ctx.g.GetLink(linkId);
        if (link) {
            return EvalPin(ctx, link->startPinId);
        }
    }
    // default
    return PinValueToValue(pin->defaultValue, pin->type);
}

static Value EvalNodeOutput(EvalCtx& ctx, const MaterialNode& node, PinID outPinId) {
    auto in = [&](size_t idx) -> Value {
        if (idx >= node.inputPins.size()) return 0.0f;
        return EvalInputPin(ctx, node.inputPins[idx]);
    };

    // Helpers for scalar outputs
    auto outIdx = [&]() -> int {
        for (int i = 0; i < (int)node.outputPins.size(); ++i)
            if (node.outputPins[i] == outPinId) return i;
        return 0;
    };

    switch (node.type) {
        case NodeType::ConstFloat: return std::holds_alternative<float>(node.parameter) ? std::get<float>(node.parameter) : 0.0f;
        case NodeType::ConstVec2: return std::holds_alternative<glm::vec2>(node.parameter) ? std::get<glm::vec2>(node.parameter) : glm::vec2(0.0f);
        case NodeType::ConstVec3: return std::holds_alternative<glm::vec3>(node.parameter) ? std::get<glm::vec3>(node.parameter) : glm::vec3(0.0f);
        case NodeType::ConstVec4: return std::holds_alternative<glm::vec4>(node.parameter) ? std::get<glm::vec4>(node.parameter) : glm::vec4(0.0f);

        case NodeType::Add: return BinaryOp(in(0), in(1), '+');
        case NodeType::Subtract: return BinaryOp(in(0), in(1), '-');
        case NodeType::Multiply: return BinaryOp(in(0), in(1), '*');
        case NodeType::Divide: return BinaryOp(in(0), in(1), '/');

        case NodeType::Lerp: {
            Value a = in(0);
            Value b = in(1);
            float t = AsFloat(in(2), 0.5f);
            const int n = std::max(Components(a), Components(b));
            glm::vec4 va = ToVec4(Convert(a, n));
            glm::vec4 vb = ToVec4(Convert(b, n));
            glm::vec4 r = (1.0f - t) * va + t * vb;
            return FromVecN(r, n);
        }

        case NodeType::Clamp: {
            float x = AsFloat(in(0));
            float lo = AsFloat(in(1), 0.0f);
            float hi = AsFloat(in(2), 1.0f);
            return std::clamp(x, lo, hi);
        }
        case NodeType::OneMinus: return 1.0f - AsFloat(in(0), 0.0f);
        case NodeType::Abs: return std::fabs(AsFloat(in(0), 0.0f));
        case NodeType::Power: return std::pow(AsFloat(in(0), 0.0f), AsFloat(in(1), 1.0f));

        case NodeType::Min: {
            Value a = in(0), b = in(1);
            const int n = std::max(Components(a), Components(b));
            glm::vec4 va = ToVec4(Convert(a, n));
            glm::vec4 vb = ToVec4(Convert(b, n));
            return FromVecN(glm::min(va, vb), n);
        }
        case NodeType::Max: {
            Value a = in(0), b = in(1);
            const int n = std::max(Components(a), Components(b));
            glm::vec4 va = ToVec4(Convert(a, n));
            glm::vec4 vb = ToVec4(Convert(b, n));
            return FromVecN(glm::max(va, vb), n);
        }
        case NodeType::Saturate: {
            Value x = in(0);
            const int n = Components(x);
            glm::vec4 v = ToVec4(Convert(x, n));
            return FromVecN(glm::clamp(v, glm::vec4(0.0f), glm::vec4(1.0f)), n);
        }
        case NodeType::Sqrt: return std::sqrt(std::max(AsFloat(in(0), 0.0f), 0.0f));
        case NodeType::Floor: return std::floor(AsFloat(in(0), 0.0f));
        case NodeType::Ceil: return std::ceil(AsFloat(in(0), 0.0f));
        case NodeType::Fract: {
            float x = AsFloat(in(0), 0.0f);
            return x - std::floor(x);
        }
        case NodeType::Mod: {
            float a = AsFloat(in(0), 0.0f);
            float b = std::max(AsFloat(in(1), 1.0f), 1e-6f);
            return std::fmod(a, b);
        }
        case NodeType::Exp: return std::exp(AsFloat(in(0), 0.0f));
        case NodeType::Log: return std::log(std::max(AsFloat(in(0), 1.0f), 1e-6f));
        case NodeType::Negate: return -AsFloat(in(0), 0.0f);

        case NodeType::Dot: return glm::dot(AsVec3(in(0)), AsVec3(in(1)));
        case NodeType::Normalize: {
            glm::vec3 v = AsVec3(in(0), glm::vec3(0.0f));
            float len = glm::length(v);
            if (len < 1e-6f) return glm::vec3(0.0f);
            return v / len;
        }
        case NodeType::Length: return glm::length(AsVec3(in(0), glm::vec3(0.0f)));

        case NodeType::SeparateVec3: {
            glm::vec3 v = AsVec3(in(0), glm::vec3(0.0f));
            int idx = outIdx();
            if (idx == 0) return v.x;
            if (idx == 1) return v.y;
            return v.z;
        }
        case NodeType::SeparateVec4: {
            glm::vec4 v = ToVec4(in(0));
            int idx = outIdx();
            if (idx == 0) return v.x;
            if (idx == 1) return v.y;
            if (idx == 2) return v.z;
            return v.w;
        }
        case NodeType::SeparateVec2: {
            glm::vec2 v = std::holds_alternative<glm::vec2>(in(0)) ? std::get<glm::vec2>(in(0)) : glm::vec2(AsFloat(in(0)));
            int idx = outIdx();
            return (idx == 0) ? v.x : v.y;
        }
        case NodeType::CombineVec3: {
            return glm::vec3(AsFloat(in(0)), AsFloat(in(1)), AsFloat(in(2)));
        }
        case NodeType::CombineVec4: {
            return glm::vec4(AsFloat(in(0)), AsFloat(in(1)), AsFloat(in(2)), AsFloat(in(3), 1.0f));
        }
        case NodeType::CombineVec2: {
            return glm::vec2(AsFloat(in(0)), AsFloat(in(1)));
        }

        case NodeType::Reroute:
            return in(0);

        // Conversions
        case NodeType::FloatToVec3: return glm::vec3(AsFloat(in(0)));
        case NodeType::Vec3ToFloat: return AsVec3(in(0)).x;
        case NodeType::Vec2ToVec3: {
            glm::vec2 v = std::holds_alternative<glm::vec2>(in(0)) ? std::get<glm::vec2>(in(0)) : glm::vec2(0.0f);
            return glm::vec3(v, AsFloat(in(1), 0.0f));
        }
        case NodeType::Vec3ToVec4: {
            glm::vec3 v = AsVec3(in(0));
            return glm::vec4(v, AsFloat(in(1), 1.0f));
        }
        case NodeType::Vec4ToVec3: {
            glm::vec4 v = ToVec4(in(0));
            return glm::vec3(v.x, v.y, v.z);
        }

        // Inputs (constants for tracer)
        case NodeType::UV: return glm::vec2(0.0f);
        case NodeType::WorldPosition: return glm::vec3(0.0f);
        case NodeType::WorldNormal: return glm::vec3(0.0f, 0.0f, 1.0f);
        case NodeType::ViewDirection: return glm::vec3(0.0f, 0.0f, 1.0f);
        case NodeType::VertexColor: return glm::vec4(1.0f);
        case NodeType::Time: return 0.0f;

        case NodeType::CustomCode: {
            // Evaluate only a simple assignment to Out (or other output name), as an expression.
            std::string code = std::holds_alternative<std::string>(node.parameter) ? std::get<std::string>(node.parameter) : std::string();

            // Build variables from input pins by name.
            std::unordered_map<std::string, Value> vars;
            vars.reserve(node.inputPins.size() + 4);
            for (PinID pid : node.inputPins) {
                const MaterialPin* p = ctx.g.GetPin(pid);
                if (!p) continue;
                vars[p->name] = EvalInputPin(ctx, pid);
            }
            // Defaults
            if (vars.find("In") == vars.end()) vars["In"] = glm::vec3(0.0f);

            // Determine which output we need (pin name).
            std::string outName = "Out";
            if (const MaterialPin* op = ctx.g.GetPin(outPinId)) outName = op->name;

            std::string expr;
            if (!ExtractAssignmentExpr(code, outName, expr)) {
                // Fallback: if no explicit assignment, passthrough Out=In.
                return vars["In"];
            }

            std::string parseErr;
            Parser p(expr, vars, &parseErr);
            Value r = p.ParseExpr();
            if (!parseErr.empty() && ctx.err && ctx.err->empty()) *ctx.err = "CustomCode parse error: " + parseErr;
            return r;
        }

        default:
            // Unsupported nodes for tracer constants: textures/noise/etc.
            // Use pin default type as a reasonable fallback.
            if (const MaterialPin* outPin = ctx.g.GetPin(outPinId)) {
                return DefaultForPinType(outPin->type);
            }
            return 0.0f;
    }
}

static Value EvalPin(EvalCtx& ctx, PinID pinId) {
    auto it = ctx.cache.find(pinId);
    if (it != ctx.cache.end()) return it->second;

    if (ctx.visiting[pinId]) {
        if (ctx.err && ctx.err->empty()) *ctx.err = "Cycle detected in tracer material evaluation";
        return 0.0f;
    }
    ctx.visiting[pinId] = true;

    const MaterialPin* pin = ctx.g.GetPin(pinId);
    if (!pin) {
        ctx.visiting[pinId] = false;
        return 0.0f;
    }

    Value v = 0.0f;
    if (pin->direction == PinDirection::Input) {
        v = EvalInputPin(ctx, pinId);
    } else {
        const MaterialNode* node = ctx.g.GetNode(pin->nodeId);
        if (node) v = EvalNodeOutput(ctx, *node, pinId);
        else v = 0.0f;
    }

    ctx.cache[pinId] = v;
    ctx.visiting[pinId] = false;
    return v;
}

} // namespace

bool EvaluateTracerConstants(const MaterialGraph& graph, TracerMaterialConstants& out, std::string& outError) {
    outError.clear();

    const MaterialNode* outNode = graph.GetNode(graph.GetOutputNodeId());
    if (!outNode || outNode->type != NodeType::PBROutput) {
        outError = "No PBROutput node";
        return false;
    }

    EvalCtx ctx{ graph };
    ctx.err = &outError;

    auto evalInputAs = [&](size_t idx, PinType desired) -> Value {
        if (idx >= outNode->inputPins.size()) return DefaultForPinType(desired);
        PinID pid = outNode->inputPins[idx];
        const MaterialPin* pin = graph.GetPin(pid);
        if (!pin) return DefaultForPinType(desired);
        Value v = EvalInputPin(ctx, pid);
        return Convert(v, GetPinTypeComponents(desired));
    };

    glm::vec3 baseColor = AsVec3(evalInputAs(0, PinType::Vec3), glm::vec3(0.8f));
    float metallic = AsFloat(evalInputAs(1, PinType::Float), 0.0f);
    float roughness = AsFloat(evalInputAs(2, PinType::Float), 0.5f);
    glm::vec3 emissive = AsVec3(evalInputAs(4, PinType::Vec3), glm::vec3(0.0f));
    float alpha = AsFloat(evalInputAs(5, PinType::Float), 1.0f);

    out.baseColor = glm::vec4(baseColor, alpha);
    out.emissive = glm::vec4(emissive, 1.0f);
    out.metallic = metallic;
    out.roughness = roughness;
    out.ior = 1.5f;
    out.flags = 0;

    // If we got a parse/eval error, still return a result but signal it.
    return outError.empty();
}

} // namespace lucent::material

