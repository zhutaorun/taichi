/*******************************************************************************
    Taichi - Physically based Computer Graphics Library

    Copyright (c) 2016 Yuanming Hu <yuanmhu@gmail.com>
                  2017 Yu Fang <squarefk@gmail.com>

    All rights reserved. Use of this source code is governed by
    the MIT license as written in the LICENSE file.
*******************************************************************************/

#include "mpm3.h"
#include <taichi/math/qr_svd.h>
#include <taichi/system/threading.h>
#include <taichi/visual/texture.h>
#include <taichi/common/asset_manager.h>
#include <taichi/math/math_util.h>

TC_NAMESPACE_BEGIN

// Note: assuming abs(x) <= 2!!
inline float w(float x) {
    x = abs(x);
    assert(x <= 2);
    if (x < 1) {
        return 0.5f * x * x * x - x * x + 2.0f / 3.0f;
    } else {
        return -1.0f / 6.0f * x * x * x + x * x - 2 * x + 4.0f / 3.0f;
    }
}

// Note: assuming abs(x) <= 2!!
inline float dw(float x) {
    float s = x < 0.0f ? -1.0f : 1.0f;
    x *= s;
    assert(x <= 2.0f);
    float val;
    float xx = x * x;
    if (x < 1.0f) {
        val = 1.5f * xx - 2.0f * x;
    } else {
        val = -0.5f * xx + 2.0f * x - 2.0f;
    }
    return s * val;
}

inline float w(const Vector3 &a) {
    return w(a.x) * w(a.y) * w(a.z);
}

inline Vector3 dw(const Vector3 &a) {
    return Vector3(dw(a.x) * w(a.y) * w(a.z), w(a.x) * dw(a.y) * w(a.z), w(a.x) * w(a.y) * dw(a.z));
}

long long MPM3D::Particle::instance_count;

struct EPParticle3 : public MPM3D::Particle {
    real hardening = 10.0f;
    real mu_0 = 1e5f, lambda_0 = 1e5f;
    real theta_c = 2.5e-2f, theta_s = 7.5e-3f;

    EPParticle3() : MPM3D::Particle() {
    }

    void initialize(const Config &config) {
        hardening = config.get("hardening", hardening);
        lambda_0 = config.get("lambda_0", lambda_0);
        mu_0 = config.get("mu_0", mu_0);
        theta_c = config.get("theta_c", theta_c);
        theta_s = config.get("theta_s", theta_s);
        real compression = config.get("compression", 1.0f);
        dg_p = Matrix(compression);
    }

    virtual Matrix get_energy_gradient() {
        real j_e = det(dg_e);
        real j_p = det(dg_p);
        real e = std::exp(std::min(hardening * (1.0f - j_p), 10.0f));
        real mu = mu_0 * e;
        real lambda = lambda_0 * e;
        Matrix r, s;
        polar_decomp(dg_e, r, s);
        if (!is_normal(r)) {
            P(dg_e);
            P(r);
            P(s);
        }
        CV(r);
        CV(s);
        return 2 * mu * (dg_e - r) +
               lambda * (j_e - 1) * j_e * glm::inverse(glm::transpose(dg_e));
    }

    virtual void calculate_kernels() {}

    virtual void calculate_force() {
        tmp_force = -vol * get_energy_gradient() * glm::transpose(dg_e);
    };

    virtual void plasticity() {
        Matrix svd_u, sig, svd_v;
        svd(dg_e, svd_u, sig, svd_v);
        for (int i = 0; i < D; i++) {
            sig[i][i] = clamp(sig[i][i], 1.0f - theta_c, 1.0f + theta_s);
        }
        dg_e = svd_u * sig * glm::transpose(svd_v);
        dg_p = glm::inverse(dg_e) * dg_cache;
        svd(dg_p, svd_u, sig, svd_v);
        for (int i = 0; i < D; i++) {
            sig[i][i] = clamp(sig[i][i], 0.1f, 10.0f);
        }
        dg_p = svd_u * sig * glm::transpose(svd_v);
    };
};

struct DPParticle3 : public MPM3D::Particle {
    real h_0 = 35.0f, h_1 = 9.0f, h_2 = 0.2f, h_3 = 10.0f;
    real lambda_0 = 204057.0f, mu_0 = 136038.0f;
    real alpha = 1.0f;
    real q = 0.0f;

    DPParticle3() : MPM3D::Particle() {
    }

    void initialize(const Config &config) {
        h_0 = config.get("h_0", h_0);
        h_1 = config.get("h_1", h_1);
        h_2 = config.get("h_2", h_2);
        h_3 = config.get("h_3", h_3);
        lambda_0 = config.get("lambda_0", lambda_0);
        mu_0 = config.get("mu_0", mu_0);
        alpha = config.get("alpha", alpha);
        real compression = config.get("compression", 1.0f);
        dg_p = Matrix(compression);
    }

    Matrix3 get_energy_gradient() {
        return Matrix3(1.f);
    }

    void project(Matrix3 sigma, real alpha, Matrix3 &sigma_out, real &out) {
        const real d = 3;
        Matrix3 epsilon(log(sigma[0][0]), 0.f, 0.f, 0.f, log(sigma[1][1]), 0.f, 0.f, 0.f, log(sigma[2][2]));
        real tr = epsilon[0][0] + epsilon[1][1] + epsilon[2][2];
        Matrix3 epsilon_hat = epsilon - (tr) / d * Matrix3(1.0f);
        real epsilon_for = sqrt(
                epsilon[0][0] * epsilon[0][0] + epsilon[1][1] * epsilon[1][1] + epsilon[2][2] * epsilon[2][2]);
        real epsilon_hat_for = sqrt(epsilon_hat[0][0] * epsilon_hat[0][0] + epsilon_hat[1][1] * epsilon_hat[1][1] +
                                    epsilon_hat[2][2] * epsilon_hat[2][2]);
        if (epsilon_hat_for <= 0 || tr > 0.0f) {
            sigma_out = Matrix3(1.0f);
            out = epsilon_for;
        } else {
            real delta_gamma = epsilon_hat_for + (d * lambda_0 + 2 * mu_0) / (2 * mu_0) * tr * alpha;
            if (delta_gamma <= 0) {
                sigma_out = sigma;
                out = 0;
            } else {
                Matrix3 h = epsilon - delta_gamma / epsilon_hat_for * epsilon_hat;
                sigma_out = Matrix3(exp(h[0][0]), 0.f, 0.f, 0.f, exp(h[1][1]), 0.f, 0.f, 0.f, exp(h[2][2]));
                out = delta_gamma;
            }
        }
    }

    void calculate_kernels() {
    }

    void calculate_force() {
        Matrix3 u, v, sig, dg = dg_e;
        svd(dg_e, u, sig, v);

        assert_info(sig[0][0] > 0, "negative singular value");
        assert_info(sig[1][1] > 0, "negative singular value");
        assert_info(sig[2][2] > 0, "negative singular value");

        Matrix3 log_sig(log(sig[0][0]), 0.f, 0.f, 0.f, log(sig[1][1]), 0.f, 0.f, 0.f, log(sig[2][2]));
        Matrix3 inv_sig(1.f / (sig[0][0]), 0.f, 0.f, 0.f, 1.f / (sig[1][1]), 0.f, 0.f, 0.f, 1.f / (sig[2][2]));
        Matrix3 center =
                2 * mu_0 * inv_sig * log_sig + lambda_0 * (log_sig[0][0] + log_sig[1][1] + log_sig[2][2]) * inv_sig;

        tmp_force = -vol * (u * center * glm::transpose(v)) * glm::transpose(dg);
    }

    void plasticity() {
        Matrix3 u, v, sig;
        svd(dg_e, u, sig, v);
        Matrix3 t = Matrix3(1.0);
        real delta_q = 0;
        project(sig, alpha, t, delta_q);
        Matrix3 rec = u * sig * glm::transpose(v);
        Matrix3 diff = rec - dg_e;
        if (!(frobenius_norm(diff) < 1e-4f)) {
            // debug code
            P(dg_e);
            P(rec);
            P(u);
            P(sig);
            P(v);
            error("SVD error\n");
        }
        dg_e = u * t * glm::transpose(v);
        dg_p = v * glm::inverse(t) * sig * glm::transpose(v) * dg_p;
        q += delta_q;
        real phi = h_0 + (h_1 * q - h_3) * expf(-h_2 * q);
        alpha = sqrtf(2.0f / 3.0f) * (2.0f * sin(phi * pi / 180.0f)) / (3.0f - sin(phi * pi / 180.0f));
    }
};

void MPM3D::initialize(const Config &config) {
    Simulation3D::initialize(config);
    res = config.get_vec3i("resolution");
    gravity = config.get_vec3("gravity");
    delta_t = config.get_real("delta_t");
    apic = config.get("apic", true);
    grid_velocity.initialize(res[0], res[1], res[2], Vector(0.0f), Vector3(0.0f));
    grid_mass.initialize(res[0], res[1], res[2], 0, Vector3(0.0f));
    grid_locks.initialize(res[0], res[1], res[2], 0, Vector3(0.0f));
}

void MPM3D::add_particles(const Config &config) {
    std::shared_ptr<Texture> density_texture = AssetManager::get_asset<Texture>(config.get_int("density_tex"));
    for (int i = 0; i < res[0]; i++) {
        for (int j = 0; j < res[1]; j++) {
            for (int k = 0; k < res[2]; k++) {
                Vector3 coord = Vector3(i + 0.5f, j + 0.5f, k + 0.5f) / Vector3(res);
                real num = density_texture->sample(coord).x;
                int t = (int)num + (rand() < num - int(num));
                for (int l = 0; l < t; l++) {
                    Particle *p = nullptr;
                    if (config.get("type", std::string("ep")) == std::string("ep")) {
                        p = new EPParticle3();
                        p->initialize(config);
                    } else {
                        p = new DPParticle3();
                        p->initialize(config);
                    }
                    p->pos = Vector(i + rand(), j + rand(), k + rand());
                    p->mass = 1.0f;
                    p->v = config.get("initial_velocity", p->v);
                    particles.push_back(p);
                }
            }
        }
    }
    P(particles.size());
}

std::vector<RenderParticle> MPM3D::get_render_particles() const {
    using Particle = RenderParticle;
    std::vector<Particle> render_particles;
    render_particles.reserve(particles.size());
    Vector3 center(res[0] / 2.0f, res[1] / 2.0f, res[2] / 2.0f);
    for (auto p_p : particles) {
        MPM3D::Particle &p = *p_p;
        render_particles.push_back(Particle(p.pos - center, Vector4(0.8f, 0.9f, 1.0f, 0.5f)));
    }
    return render_particles;
}

void MPM3D::rasterize() {
    grid_velocity.reset(Vector(0.0f));
    grid_mass.reset(0.0f);
    parallel_for_each_particle([&](Particle &p) {
        for (auto &ind : get_bounded_rasterization_region(p.pos)) {
            Vector3 d_pos = Vector(ind.i, ind.j, ind.k) - p.pos;
            real weight = w(d_pos);
            grid_locks[ind].lock();
            grid_mass[ind] += weight * p.mass;
            grid_velocity[ind] += weight * p.mass * (p.v + (3.0f) * p.apic_b * d_pos);
            grid_locks[ind].unlock();
        }
    });
    for (auto ind : grid_mass.get_region()) {
        if (grid_mass[ind] > 0) {
            CV(grid_velocity[ind]);
            CV(1 / grid_mass[ind]);
            grid_velocity[ind] = grid_velocity[ind] * (1.0f / grid_mass[ind]);
            CV(grid_velocity[ind]);
        }
    }
}

void MPM3D::resample(float delta_t) {
    real alpha_delta_t = 1;
    // what is apic in 2D
    if (apic)
        alpha_delta_t = 0;
    parallel_for_each_particle([&](Particle &p) {
        Vector v(0.0f), bv(0.0f);
        Matrix cdg(0.0f);
        Matrix b(0.0f);
        int count = 0;
        for (auto &ind : get_bounded_rasterization_region(p.pos)) {
            count++;
            Vector d_pos = p.pos - Vector3(ind.i, ind.j, ind.k);
            float weight = w(d_pos);
            Vector gw = dw(d_pos);
            Vector grid_vel = grid_velocity[ind];
            v += weight * grid_vel;
            Vector aa = grid_vel;
            Vector bb = -d_pos;
            Matrix out(aa[0] * bb[0], aa[1] * bb[0], aa[2] * bb[0],
                       aa[0] * bb[1], aa[1] * bb[1], aa[2] * bb[1],
                       aa[0] * bb[2], aa[1] * bb[2], aa[2] * bb[2]);
            b += weight * out;
            bv += weight * grid_velocity_backup[ind];
            cdg += glm::outerProduct(grid_velocity[ind], gw);
            CV(grid_velocity[ind]);
        }
        if (count != 64 || !apic) {
            b = Matrix(0);
        }
        p.apic_b = b;
        cdg = Matrix(1) + delta_t * cdg;
        p.v = (1 - alpha_delta_t) * v + alpha_delta_t * (v - bv + p.v);
        Matrix dg = cdg * p.dg_e * p.dg_p;
        p.dg_e = cdg * p.dg_e;
        p.dg_cache = dg;
    });
}

void MPM3D::apply_deformation_force(float delta_t) {
    //printf("Calculating force...\n");
    parallel_for_each_particle([&](Particle &p) {
        p.calculate_force();
    });
    //printf("Accumulating force...\n");
    parallel_for_each_particle([&](Particle &p) {
        for (auto &ind : get_bounded_rasterization_region(p.pos)) {
            real mass = grid_mass[ind];
            if (mass == 0.0f) { // No EPS here
                continue;
            }
            Vector d_pos = p.pos - Vector3(ind.i, ind.j, ind.k);
            Vector gw = dw(d_pos);
            Vector force = p.tmp_force * gw;
            CV(force);
            grid_locks[ind].lock();
            grid_velocity[ind] += delta_t / mass * force;
            grid_locks[ind].unlock();
        }
    });
}

void MPM3D::grid_apply_boundary_conditions(const DynamicLevelSet3D &levelset, real t) {
    for (auto &ind : grid_velocity.get_region()) {
        Vector3 pos = Vector3(ind.get_pos());
        real phi = levelset.sample(pos, t);
        if (phi > 1) continue;
        Vector3 n = levelset.get_spatial_gradient(pos, t);
        Vector boundary_velocity = levelset.get_temporal_derivative(pos, t) * n;
        Vector3 v = grid_velocity[ind] - boundary_velocity;
        if (phi > 0) { // 0~1
            real pressure = std::max(-glm::dot(v, n), 0.0f);
            real mu = levelset.levelset0->friction;
            if (mu < 0) { // sticky
                v = Vector3(0.0f);
            } else {
                Vector3 t = v - n * glm::dot(v, n);
                if (length(t) > 1e-6f) {
                    t = normalize(t);
                }
                real friction = -clamp(glm::dot(t, v), -mu * pressure, mu * pressure);
                v = v + n * pressure + t * friction;
            }
        } else if (phi <= 0) {
            v = Vector3(0.0f);
        }
        v += boundary_velocity;
        grid_velocity[ind] = v;
    }
}

void MPM3D::particle_collision_resolution(real t) {
    parallel_for_each_particle([&](Particle &p) {
        p.resolve_collision(levelset, t);
    });
}

void MPM3D::substep(float delta_t) {
    if (!particles.empty()) {
        /*
        for (auto &p : particles) {
            p.calculate_kernels();
        }
        */
//        apply_external_impulse(gravity * delta_t);
        rasterize();
        grid_backup_velocity();
        grid_apply_external_force(gravity, delta_t);
        apply_deformation_force(delta_t);
        grid_apply_boundary_conditions(levelset, current_t);
        resample(delta_t);
        parallel_for_each_particle([&](Particle &p) {
            p.pos += delta_t * p.v;
            p.pos.x = clamp(p.pos.x, 0.0f, res[0] - eps);
            p.pos.y = clamp(p.pos.y, 0.0f, res[1] - eps);
            p.pos.z = clamp(p.pos.z, 0.0f, res[2] - eps);
            p.plasticity();
        });
        particle_collision_resolution(current_t);
    }
    current_t += delta_t;
}

bool MPM3D::test() const {
    for (int i = 0; i < 100000; i++) {
        Matrix3 m(1.000000238418579101562500000000, -0.000000000000000000000000000000,
                  -0.000000000000000000000220735070, 0.000000000000000000000000000000, 1.000000238418579101562500000000,
                  -0.000000000000000000216840434497, 0.000000000000000000000211758237,
                  -0.000000000000000001084202172486, 1.000000000000000000000000000000);
        Matrix3 u, sig, v;
        svd(m, u, sig, v);
        if (!is_normal(sig)) {
            P(m);
            P(u);
            P(sig);
            P(v);
        }
    }
    return false;
}

TC_IMPLEMENTATION(Simulation3D, MPM3D, "mpm");

TC_NAMESPACE_END
