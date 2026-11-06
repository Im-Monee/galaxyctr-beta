// Portable implementation of the Wii matrix library (revolution/mtx.h).
// The PS* functions use paired-single assembly on the Wii; these give the same results in C.
// The C_* functions are copied from the decompiled SDK (src/RVL_SDK/mtx).

#include <revolution/mtx.h>
#include <math.h>

extern "C" {

void PSMTXIdentity(Mtx m) {
    m[0][0] = 1.0f;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = 1.0f;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 1.0f;
    m[2][3] = 0.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) {
    if (src == dst) {
        return;
    }

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            dst[i][j] = src[i][j];
        }
    }
}

// ab = a * b, treating both as 4x4 matrices with an implicit last row of (0, 0, 0, 1).
void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    Mtx tmp;

    for (int i = 0; i < 3; i++) {
        tmp[i][0] = a[i][0] * b[0][0] + a[i][1] * b[1][0] + a[i][2] * b[2][0];
        tmp[i][1] = a[i][0] * b[0][1] + a[i][1] * b[1][1] + a[i][2] * b[2][1];
        tmp[i][2] = a[i][0] * b[0][2] + a[i][1] * b[1][2] + a[i][2] * b[2][2];
        tmp[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
    }

    PSMTXCopy(tmp, ab);
}

u32 PSMTXInverse(const Mtx src, Mtx inv) {
    f32 det = src[0][0] * src[1][1] * src[2][2] + src[0][1] * src[1][2] * src[2][0] + src[0][2] * src[1][0] * src[2][1] -
              src[2][0] * src[1][1] * src[0][2] - src[1][0] * src[0][1] * src[2][2] - src[0][0] * src[2][1] * src[1][2];

    if (det == 0.0f) {
        return 0;
    }

    det = 1.0f / det;
    Mtx tmp;
    tmp[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * det;
    tmp[0][1] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * det;
    tmp[0][2] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * det;
    tmp[1][0] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * det;
    tmp[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * det;
    tmp[1][2] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * det;
    tmp[2][0] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * det;
    tmp[2][1] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * det;
    tmp[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * det;
    tmp[0][3] = -tmp[0][0] * src[0][3] - tmp[0][1] * src[1][3] - tmp[0][2] * src[2][3];
    tmp[1][3] = -tmp[1][0] * src[0][3] - tmp[1][1] * src[1][3] - tmp[1][2] * src[2][3];
    tmp[2][3] = -tmp[2][0] * src[0][3] - tmp[2][1] * src[1][3] - tmp[2][2] * src[2][3];

    PSMTXCopy(tmp, inv);
    return 1;
}

void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    Vec tmp;
    tmp.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z + m[0][3];
    tmp.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z + m[1][3];
    tmp.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z + m[2][3];
    *dst = tmp;
}

// Rotation and scale only: the translation column is ignored.
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    Vec tmp;
    tmp.x = m[0][0] * src->x + m[0][1] * src->y + m[0][2] * src->z;
    tmp.y = m[1][0] * src->x + m[1][1] * src->y + m[1][2] * src->z;
    tmp.z = m[2][0] * src->x + m[2][1] * src->y + m[2][2] * src->z;
    *dst = tmp;
}

void PSMTXMultVecArraySR(const Mtx m, const Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; i++) {
        PSMTXMultVecSR(m, &srcBase[i], &dstBase[i]);
    }
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    PSMTXIdentity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    PSMTXCopy(src, dst);
    dst[0][3] += xT;
    dst[1][3] += yT;
    dst[2][3] += zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    PSMTXIdentity(m);
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

// Scales each row (including its translation) by the matching factor: dst = scale * src.
void PSMTXScaleApply(const Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    const f32 scale[3] = {xS, yS, zS};

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            dst[i][j] = src[i][j] * scale[i];
        }
    }
}

void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) {
    switch (axis | 0x20) {
    case 'x':
        PSMTXIdentity(m);
        m[1][1] = cosA;
        m[1][2] = -sinA;
        m[2][1] = sinA;
        m[2][2] = cosA;
        break;
    case 'y':
        PSMTXIdentity(m);
        m[0][0] = cosA;
        m[0][2] = sinA;
        m[2][0] = -sinA;
        m[2][2] = cosA;
        break;
    case 'z':
        PSMTXIdentity(m);
        m[0][0] = cosA;
        m[0][1] = -sinA;
        m[1][0] = sinA;
        m[1][1] = cosA;
        break;
    default:
        // The original leaves the matrix untouched for an unknown axis.
        break;
    }
}

void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 rad) {
    f32 s = sinf(rad);
    f32 c = cosf(rad);
    f32 t = 1.0f - c;

    Vec v;
    PSVECNormalize(axis, &v);

    f32 x = v.x;
    f32 y = v.y;
    f32 z = v.z;

    m[0][0] = t * x * x + c;
    m[0][1] = t * x * y - s * z;
    m[0][2] = t * x * z + s * y;
    m[0][3] = 0.0f;
    m[1][0] = t * x * y + s * z;
    m[1][1] = t * y * y + c;
    m[1][2] = t * y * z - s * x;
    m[1][3] = 0.0f;
    m[2][0] = t * x * z - s * y;
    m[2][1] = t * y * z + s * x;
    m[2][2] = t * z * z + c;
    m[2][3] = 0.0f;
}

void PSMTXQuat(Mtx m, const Quaternion* q) {
    f32 s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);

    f32 xs = q->x * s;
    f32 ys = q->y * s;
    f32 zs = q->z * s;
    f32 wx = q->w * xs;
    f32 wy = q->w * ys;
    f32 wz = q->w * zs;
    f32 xx = q->x * xs;
    f32 xy = q->x * ys;
    f32 xz = q->x * zs;
    f32 yy = q->y * ys;
    f32 yz = q->y * zs;
    f32 zz = q->z * zs;

    m[0][0] = 1.0f - (yy + zz);
    m[0][1] = xy - wz;
    m[0][2] = xz + wy;
    m[0][3] = 0.0f;
    m[1][0] = xy + wz;
    m[1][1] = 1.0f - (xx + zz);
    m[1][2] = yz - wx;
    m[1][3] = 0.0f;
    m[2][0] = xz - wy;
    m[2][1] = yz + wx;
    m[2][2] = 1.0f - (xx + yy);
    m[2][3] = 0.0f;
}

void PSMTX44Identity(Mtx44 m) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m[i][j] = i == j ? 1.0f : 0.0f;
        }
    }
}

void PSMTX44Copy(const Mtx44 src, Mtx44 dst) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            dst[i][j] = src[i][j];
        }
    }
}

void PSVECAdd(const Vec* a, const Vec* b, Vec* ab) {
    ab->x = a->x + b->x;
    ab->y = a->y + b->y;
    ab->z = a->z + b->z;
}

void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b) {
    a_b->x = a->x - b->x;
    a_b->y = a->y - b->y;
    a_b->z = a->z - b->z;
}

void PSVECScale(const Vec* src, Vec* dst, f32 scale) {
    dst->x = src->x * scale;
    dst->y = src->y * scale;
    dst->z = src->z * scale;
}

void PSVECNormalize(const Vec* src, Vec* unit) {
    f32 inv = 1.0f / sqrtf(src->x * src->x + src->y * src->y + src->z * src->z);
    unit->x = src->x * inv;
    unit->y = src->y * inv;
    unit->z = src->z * inv;
}

f32 PSVECMag(const Vec* v) {
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb) {
    Vec tmp;
    tmp.x = a->y * b->z - a->z * b->y;
    tmp.y = a->z * b->x - a->x * b->z;
    tmp.z = a->x * b->y - a->y * b->x;
    *axb = tmp;
}

f32 PSVECDistance(const Vec* a, const Vec* b) {
    f32 dx = a->x - b->x;
    f32 dy = a->y - b->y;
    f32 dz = a->z - b->z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

f32 C_VECMag(const Vec* v) {
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}

void PSQUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* pq) {
    Quaternion tmp;
    tmp.w = p->w * q->w - p->x * q->x - p->y * q->y - p->z * q->z;
    tmp.x = p->w * q->x + p->x * q->w + p->y * q->z - p->z * q->y;
    tmp.y = p->w * q->y + p->y * q->w + p->z * q->x - p->x * q->z;
    tmp.z = p->w * q->z + p->z * q->w + p->x * q->y - p->y * q->x;
    *pq = tmp;
}

f32 PSQUATDotProduct(const Quaternion* p, const Quaternion* q) {
    return p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
}

void C_MTXLookAt(Mtx m, const Point3d* camPos, const Vec* camUp, const Point3d* target) {
    Vec vLook, vRight, vUp;

    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    PSVECNormalize(&vLook, &vLook);
    PSVECCrossProduct(camUp, &vLook, &vRight);
    PSVECNormalize(&vRight, &vRight);
    PSVECCrossProduct(&vLook, &vRight, &vUp);

    m[0][0] = vRight.x;
    m[0][1] = vRight.y;
    m[0][2] = vRight.z;
    m[0][3] = -(camPos->x * vRight.x + camPos->y * vRight.y + camPos->z * vRight.z);
    m[1][0] = vUp.x;
    m[1][1] = vUp.y;
    m[1][2] = vUp.z;
    m[1][3] = -(camPos->x * vUp.x + camPos->y * vUp.y + camPos->z * vUp.z);
    m[2][0] = vLook.x;
    m[2][1] = vLook.y;
    m[2][2] = vLook.z;
    m[2][3] = -(camPos->x * vLook.x + camPos->y * vLook.y + camPos->z * vLook.z);
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 angle = MTXDegToRad(fovY * 0.5f);
    f32 cot = 1.0f / static_cast< f32 >(tan(angle));

    m[0][0] = (cot / aspect) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT, f32 transS, f32 transT) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = (2.0f * tmp * scaleS);
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = ((-(r + l) * tmp) * scaleS) + transS;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * tmp) * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = ((-(t + b) * tmp) * scaleT) + transT;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 angle = MTXDegToRad(fovY * 0.5f);
    f32 cot = 1.0f / static_cast< f32 >(tan(angle));

    m[0][0] = cot / aspect;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;

    f32 tmp = 1.0f / (f - n);
    m[2][2] = -(n)*tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = -(r + l) * tmp;

    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp;
    m[1][2] = 0.0f;
    m[1][3] = -(t + b) * tmp;

    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][2] = -(1.0f) * tmp;
    m[2][3] = -(f)*tmp;

    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;
}

void C_QUATMtx(Quaternion* r, const Mtx m) {
    static const s32 nxt[3] = {1, 2, 0};
    f32 q[3];
    f32 tr = m[0][0] + m[1][1] + m[2][2];

    if (tr > 0.0f) {
        tr = static_cast< f32 >(sqrt(1.0f + tr));
        r->w = tr * 0.5f;
        f32 s = 0.5f / tr;
        r->x = (m[2][1] - m[1][2]) * s;
        r->y = (m[0][2] - m[2][0]) * s;
        r->z = (m[1][0] - m[0][1]) * s;
    } else {
        s32 i = 0;
        if (m[1][1] > m[0][0]) {
            i = 1;
        }
        if (m[2][2] > m[i][i]) {
            i = 2;
        }
        s32 j = nxt[i];
        s32 k = nxt[j];
        f32 s = static_cast< f32 >(sqrt((m[i][i] - (m[j][j] + m[k][k])) + 1.0f));
        q[i] = s * 0.5f;

        if (s != 0.0f) {
            s = 0.5f / s;
        }

        r->w = (m[k][j] - m[j][k]) * s;
        q[j] = (m[i][j] + m[j][i]) * s;
        q[k] = (m[i][k] + m[k][i]) * s;

        r->x = q[0];
        r->y = q[1];
        r->z = q[2];
    }
}

void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* r, f32 t) {
    f32 tp, tq;
    f32 cos_th = p->x * q->x + p->y * q->y + p->z * q->z + p->w * q->w;
    tq = 1.0f;

    if (cos_th < 0.0f) {
        cos_th = -cos_th;
        tq = -tq;
    }

    if (cos_th <= 1.0f - 0.00001f) {
        f32 theta = acos(cos_th);
        f32 sin_th = sin(theta);
        tp = static_cast< f32 >(sin((1.0f - t) * theta)) / sin_th;
        tq *= static_cast< f32 >(sin(t * theta)) / sin_th;
    } else {
        tp = 1.0f - t;
        tq = tq * t;
    }

    r->x = tp * p->x + tq * q->x;
    r->y = tp * p->y + tq * q->y;
    r->z = tp * p->z + tq * q->z;
    r->w = tp * p->w + tq * q->w;
}

}  // extern "C"
