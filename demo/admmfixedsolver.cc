#include "admmfixedsolver.h"

// C++ standard library
#include <iostream>
#include <limits>
#include <vector>

#include "igl/slice.h"
#include "igl/svd3x3/polar_svd3x3.h"

//#define USE_TEST_FUNCTIONS
#define USE_LINEAR_SOLVE_1
//#define USE_LINEAR_SOLVE_2

namespace arap {
namespace demo {

const double kMatrixDiffThreshold = 1e-6;
const double kEnergyTolerance = 0.02;

AdmmFixedSolver::AdmmFixedSolver(const Eigen::MatrixXd& vertices,
    const Eigen::MatrixXi& faces, const Eigen::VectorXi& fixed,
    int max_iteration, double rho)
  : Solver(vertices, faces, fixed, max_iteration),
  rho_(rho) {
}

void AdmmFixedSolver::Precompute() {
  int vertex_num = vertices_.rows();
  int face_num = faces_.rows();
  int fixed_num = fixed_.size();
  int free_num = free_.size();

  // Compute weight_.
  weight_.resize(vertex_num, vertex_num);
  // An index map to help mapping from one vertex to the corresponding edge.
  int index_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  // Loop over all the faces.
  for (int f = 0; f < face_num; ++f) {
    // Get the cotangent value with that face.
    Eigen::Vector3d cotangent = ComputeCotangent(f);
    // Loop over the three vertices within the same triangle.
    // i = 0 => A.
    // i = 1 => B.
    // i = 2 => C.
    for (int i = 0; i < 3; ++i) {
      // Indices of the two vertices in the edge corresponding to vertex i.
      int first = faces_(f, index_map[i][0]);
      int second = faces_(f, index_map[i][1]);
      double half_cot = cotangent(i) / 2.0;
      weight_.coeffRef(first, second) += half_cot;
      weight_.coeffRef(second, first) += half_cot;
      // Note that weight_(i, i) is the sum of all the -weight_(i, j).
      weight_.coeffRef(first, first) -= half_cot;
      weight_.coeffRef(second, second) -= half_cot;
    }
  }

  // Compute the left matrix M_.
  // The dimension of M_ should be # of constraints by # of unknowns. i.e.,
  // (free_num + 3 * vertex_num) * (free_num + 3 * vertex_num).
  // The arrangement is as follows:
  // variables(columns in M_):
  // col(i): vertex position for p_i.
  // col(free_num + 3 * i : free_num + 3 * i + 2): the first to third
  // columns in R_i.
  // constraints(rows in M_):
  // row(0 : free_num - 1): constraints for each vertex.
  // row(free_num : free_num + 3 * vertex_num - 1): constraints for rotations.
  // Note that the problem can be decomposed to solve in three dimensions.

  // Here we have two methods to compute M_. The first one is to compute the
  // gradient directly. Unfortunately this is harder to program and check. So
  // we comment it out, and keep it here just for reference.
#ifdef USE_LINEAR_SOLVE_1
  // Start of the first method: compute the gradient directly.
  M_.resize(free_num + 3 * vertex_num, free_num + 3 * vertex_num);
  for (int i = free_num; i < free_num + 3 * vertex_num; ++i) {
    M_.coeffRef(i, i) += rho_;
  }
  int edge_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  for (int f = 0; f < face_num; ++f) {
    // Loop over all the edges.
    for (int e = 0; e < 3; ++e) {
      int first = faces_(f, edge_map[e][0]);
      int second = faces_(f, edge_map[e][1]);
      // Each edge is visited twice: i->j and j->i.
      VertexType first_type = vertex_info_[first].type;
      VertexType second_type = vertex_info_[second].type;
      int first_pos = vertex_info_[first].pos;
      int second_pos = vertex_info_[second].pos;
      double weight = weight_.coeff(first, second);
      Eigen::Vector3d v = vertices_.row(first) - vertices_.row(second);
      if (first_type == VertexType::Free) {
        // This term contributes to the gradient of first.
        M_.coeffRef(first_pos, first_pos) += 2 * weight;
        if (second_type == VertexType::Free) {
          M_.coeffRef(first_pos, second_pos) -= 2 * weight;
        }
        for (int i = 0; i < 3; ++i) {
          M_.coeffRef(first_pos, GetMatrixVariablePos(first, i))
            -= 2 * weight * v(i);
        }
      }
      if (second_type == VertexType::Free) {
        // This term contributes to the gradient of second.
        M_.coeffRef(second_pos, second_pos) += 2 * weight;
        if (first_type == VertexType::Free) {
          M_.coeffRef(second_pos, first_pos) -= 2 * weight;
        }
        for (int i = 0; i < 3; ++i) {
          M_.coeffRef(second_pos, GetMatrixVariablePos(first, i))
            += 2 * weight * v(i);
        }
      }
      // This term also contributes to R_first.
      Eigen::Matrix3d m = v * v.transpose() * weight * 2;
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
          M_.coeffRef(free_num + 3 * first + i, free_num + 3 * first + j)
            += m(i, j);
        }
      }
      if (first_type == VertexType::Free) {
        for (int i = 0; i < 3; ++i) {
          double val = weight * 2 * v(i);
          M_.coeffRef(GetMatrixVariablePos(first, i), first_pos) -= val;
        }
      }
      if (second_type == VertexType::Free) {
        for (int i = 0; i < 3; ++i) {
          double val = weight * 2 * v(i);
          M_.coeffRef(GetMatrixVariablePos(first, i), second_pos) += val;
        }
      }
    }
  }
  // End of computing the gradient directly. ***/
#endif

  // The second method looks a lot nicer: First let's consider taking the
  // derivatives of f(x) = w||Ax-b||^2:
  // f(x) = w(x'A'-b')(Ax-b) = w(x'A'Ax-2b'Ax+b'b), so
  // \nabla f(x) = w(2A'Ax-2b'A) = (2wA'A)x-2wb'A
  // So we can sum up all these terms to get the normal equation!
  // (\sum 2wA'A)x = \sum 2wA'b

#ifdef USE_LINEAR_SOLVE_2
  // Start of the second method.
  M_.resize(free_num + 3 * vertex_num, free_num + 3 * vertex_num);
  // Loop over all the edges.
  int edge_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  for (int f = 0; f < face_num; ++f) {
    // Loop over all the edges.
    for (int e = 0; e < 3; ++e) {
      int first = faces_(f, edge_map[e][0]);
      int second = faces_(f, edge_map[e][1]);
      // Check the type of first and second.
      VertexType first_type = vertex_info_[first].type;
      VertexType second_type = vertex_info_[second].type;
      // What is p_1 - p_2?
      Eigen::Vector3d v = vertices_.row(first) - vertices_.row(second);
      // What is the weight?
      double weight = weight_.coeff(first, second);
      // What is the dimension of A?
      // A is a one line sparse row vector!
      Eigen::SparseMatrix<double> A;
      A.resize(1, free_num + 3 * vertex_num);
      // Compute A.
      if (first_type == VertexType::Free) {
        A.coeffRef(0, vertex_info_[first].pos) = 1;
      }
      if (second_type == VertexType::Free) {
        A.coeffRef(0, vertex_info_[second].pos) = -1;
      }
      A.coeffRef(0, GetMatrixVariablePos(first, 0)) = -v(0);
      A.coeffRef(0, GetMatrixVariablePos(first, 1)) = -v(1);
      A.coeffRef(0, GetMatrixVariablePos(first, 2)) = -v(2);
      // Add A to M_.
      M_ = M_ + 2 * weight * A.transpose() * A;
    }
  }
  // Add the rotation constraints.
  for (int v = 0; v < vertex_num; ++v) {
    // What is A?
    Eigen::SparseMatrix<double> A;
    A.resize(3, free_num + 3 * vertex_num);
    A.coeffRef(0, GetMatrixVariablePos(v, 0)) = 1;
    A.coeffRef(1, GetMatrixVariablePos(v, 1)) = 1;
    A.coeffRef(2, GetMatrixVariablePos(v, 2)) = 1;
    // Add A to M_.
    M_ = M_ + rho_ * A.transpose() * A;
  }
  // End of the second method. ***/
#endif
  // Post-processing: compress M_, factorize it.
  M_.makeCompressed();

  // Cholesky factorization.
  solver_.compute(M_);
  if (solver_.info() != Eigen::Success) {
    // Failed to decompose M_.
    std::cout << "Fail to do Cholesky factorization." << std::endl;
    exit(EXIT_FAILURE);
  }
}

void AdmmFixedSolver::SolvePreprocess(const Eigen::MatrixXd& fixed_vertices) {
  // Cache fixed_vertices.
  fixed_vertices_ = fixed_vertices;
  // Check the dimension is correct.
  int fixed_num = fixed_.size();
  if (fixed_num != fixed_vertices_.rows()) {
    std::cout << "Wrong dimension in fixed number." << std::endl;
    exit(EXIT_FAILURE);
  }
  // Initialize with vertices_.
  vertices_updated_ = vertices_;
  // Enforce the fixed vertices in vertices_updated_.
  for (int i = 0; i < fixed_num; ++i) {
    int pos = fixed_(i);
    vertices_updated_.row(pos) = fixed_vertices_.row(i);
  }
  // Initialize rotations_ with Identity matrices.
  rotations_.clear();
  int vertex_num = vertices_.rows();
  rotations_.resize(vertex_num, Eigen::Matrix3d::Identity());
  // Initialize S_ with Identity matrices.
  S_.clear();
  S_.resize(vertex_num, Eigen::Matrix3d::Identity());
  // Initialize T_ with zero matrices.
  T_.clear();
  T_.resize(vertex_num, Eigen::Matrix3d::Zero());
}

void AdmmFixedSolver::SolveOneIteration() {
  int fixed_num = fixed_.size();
  int free_num = free_.size();
  int vertex_num = vertices_.rows();
  int face_num = faces_.rows();

  // The iteration contains four steps:
  // Step 1: linear solve.
  // Step 2: SVD solve.
  // Step 3: update T.

  // Step 1: linear solve.
  // Note that the problem can be decomposed in three dimensions.
  // The number of constraints are free_num + 3 * vertex_num.
  // The first free_num constraints are for vertex, and the remaining
  // 3 * vertex_num constraints are for matrices.
  // Similarly, we implement two methods and cross check both of them.
#ifdef USE_LINEAR_SOLVE_1
  // Method 1: Compute the derivatives directly.
  Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(free_num + 3 * vertex_num, 3);
  // Build rhs.
  // For rotation matrix constraints.
  for (int v = 0; v < vertex_num; ++v) {
    rhs.block<3, 3>(free_num + 3 * v, 0)
      = rho_ * (S_[v] - T_[v]).transpose();
  }
  int edge_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  for (int f = 0; f < face_num; ++f) {
    // Loop over all the edges.
    for (int e = 0; e < 3; ++e) {
      int first = faces_(f, edge_map[e][0]);
      int second = faces_(f, edge_map[e][1]);
      // Each edge is visited twice: i->j and j->i.
      VertexType first_type = vertex_info_[first].type;
      VertexType second_type = vertex_info_[second].type;
      int first_pos = vertex_info_[first].pos;
      int second_pos = vertex_info_[second].pos;
      double weight = weight_.coeff(first, second);
      Eigen::Vector3d v = vertices_.row(first) - vertices_.row(second);
      if (first_type == VertexType::Free && second_type == VertexType::Free) {
        continue;
      }
      if (first_type == VertexType::Free) {
        // This term contributes to the first position.
        // second_type == Fixed.
        rhs.row(first_pos) += 2 * weight * vertices_updated_.row(second);
      }
      if (second_type == VertexType::Free) {
        // This term contributes to the second position.
        // first_type == Fixed.
        rhs.row(second_pos) += 2 * weight * vertices_updated_.row(first);
      }
      // This term also contributes to the gradient of R_first.
      Eigen::Vector3d b = Eigen::Vector3d::Zero();
      if (first_type == VertexType::Fixed) {
        b += vertices_updated_.row(first);
      }
      if (second_type == VertexType::Fixed) {
        b -= vertices_updated_.row(second);
      }
      Eigen::Matrix3d m = v * b.transpose() * 2 * weight;
      rhs.block<3, 3>(GetMatrixVariablePos(first, 0), 0) += m;
    }
  }
  // End of Method 1. ***/
#endif

#ifdef USE_LINEAR_SOLVE_2
  // Method 2:
  Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(free_num + 3 * vertex_num, 3);
  // f(x) = w||Ax-b||^2:
  // (\sum 2wA'A)x = \sum 2wA'b
  // Add edge term.
  // Loop over all the edges.
  int edge_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  for (int f = 0; f < face_num; ++f) {
    // Loop over all the edges.
    for (int e = 0; e < 3; ++e) {
      int first = faces_(f, edge_map[e][0]);
      int second = faces_(f, edge_map[e][1]);
      // Check the type of first and second.
      VertexType first_type = vertex_info_[first].type;
      VertexType second_type = vertex_info_[second].type;
      // What is p_1 - p_2?
      Eigen::Vector3d v = vertices_.row(first) - vertices_.row(second);
      // What is the weight?
      double weight = weight_.coeff(first, second);
      // What is the dimension of A?
      // A is a one line sparse row vector!
      Eigen::SparseMatrix<double> A;
      A.resize(1, free_num + 3 * vertex_num);
      // Compute A.
      if (first_type == VertexType::Free) {
        A.coeffRef(0, vertex_info_[first].pos) = 1;
      }
      if (second_type == VertexType::Free) {
        A.coeffRef(0, vertex_info_[second].pos) = -1;
      }
      A.coeffRef(0, GetMatrixVariablePos(first, 0)) = -v(0);
      A.coeffRef(0, GetMatrixVariablePos(first, 1)) = -v(1);
      A.coeffRef(0, GetMatrixVariablePos(first, 2)) = -v(2);
      // What is the dimension of b?
      Eigen::Vector3d b = Eigen::Vector3d::Zero();
      if (first_type == VertexType::Fixed) {
        b -= vertices_updated_.row(first);
      }
      if (second_type == VertexType::Fixed) {
        b += vertices_updated_.row(second);
      }
      rhs += 2 * weight * A.transpose() * b.transpose();
    }
  }

  // Add the rotation constraints.
  for (int v = 0; v < vertex_num; ++v) {
    // What is A?
    Eigen::SparseMatrix<double> A;
    A.resize(3, free_num + 3 * vertex_num);
    A.coeffRef(0, GetMatrixVariablePos(v, 0)) = 1;
    A.coeffRef(1, GetMatrixVariablePos(v, 1)) = 1;
    A.coeffRef(2, GetMatrixVariablePos(v, 2)) = 1;
    // What is b?
    Eigen::Matrix3d B = (S_[v] - T_[v]).transpose();
    rhs += (rho_ * A.transpose() * B);
  }
  // End of Method 2. ***/
#endif
  // The two methods have been double checked with each other, it turns out
  // they both give the same rhs! We are free to use either of them (Probably
  // tend to use Method 1 here because it looks shorter).

  // Solve.
  Eigen::MatrixXd solution = solver_.solve(rhs);
  if (solver_.info() != Eigen::Success) {
    std::cout << "Fail to solve the sparse linear system." << std::endl;
    exit(EXIT_FAILURE);
  }
  // Sanity check the dimension of the solution.
  if (solution.rows() != free_num + 3 * vertex_num) {
    std::cout << "Fail to write back solution: dimension mismatch."
      << std::endl;
    exit(EXIT_FAILURE);
  }
  // Sanity check the value of the solution.
  if ((M_ * solution - rhs).squaredNorm() > kMatrixDiffThreshold) {
    std::cout << "Sparse linear solver is wrong!" << std::endl;
    exit(EXIT_FAILURE);
  }
  // Write back the solutions.
  for (int i = 0; i < free_num; ++i) {
    vertices_updated_.row(free_(i)) = solution.row(i);
  }
  for (int v = 0; v < vertex_num; ++v) {
    rotations_[v] = solution.block<3, 3>(free_num + 3 * v, 0).transpose();
  }
#ifdef USE_TEST_FUNCTIONS
  // Sanity check whether it is really the optimal solution!
  if (!CheckLinearSolve()) {
    std::cout << "Iteration terminated due to the linear solve error."
      << std::endl;
    exit(EXIT_FAILURE);
  }
#endif

  // Step 2: SVD solve.
#ifdef USE_TEST_FUNCTIONS
  // Compute the energy before optimization.
  const double infty = std::numeric_limits<double>::infinity();
  const double energy_before_svd = ComputeSVDSolveEnergy();
#endif
  // Input: R, S_, T_.
  // Output: S_.
  // The solution can be found in wikipedia:
  // http://en.wikipedia.org/wiki/Orthogonal_Procrustes_problem
  // R + T = U\Sigma V' S = UV'
  // The problem is (for reference):
  // \min_{S} \|S - (R + T)\|^2 s.t. S\in SO(3)
  // i.e., given R + T, find the closest SO(3) matrix.
  for (int i = 0; i < vertex_num; ++i) {
    Eigen::Matrix3d rotation;
    Eigen::Matrix3d res = rotations_[i] + T_[i];
    igl::polar_svd3x3(res, rotation);
    S_[i] = rotation;
  }
#ifdef USE_TEST_FUNCTIONS
  const double energy_after_svd = ComputeSVDSolveEnergy();
  // Test whether the energy really decreases.
  if (energy_before_svd == infty || energy_after_svd == infty
      || energy_before_svd < energy_after_svd - kEnergyTolerance) {
    std::cout << "Iteration terminated due to the svd solve error."
      << std::endl << "Energy before SVD: " << energy_before_svd
      << std::endl << "Energy after SVD: " << energy_after_svd
      << std::endl;
    exit(EXIT_FAILURE);
  }
#endif

  // Step 3: update T.
  for (int i = 0; i < vertex_num; ++i) {
    T_[i] += rotations_[i] - S_[i];
  }
}

Eigen::Vector3d AdmmFixedSolver::ComputeCotangent(int face_id) const {
  Eigen::Vector3d cotangent(0.0, 0.0, 0.0);
  // The triangle is defined as follows:
  //            A
  //           /  -
  //        c /     - b
  //         /        -
  //        /    a      -
  //       B--------------C
  // where A, B, C corresponds to faces_(face_id, 0), faces_(face_id, 1) and
  // faces_(face_id, 2). The return value is (cotA, cotB, cotC).
  // Compute the triangle area first.
  Eigen::Vector3d A = vertices_.row(faces_(face_id, 0));
  Eigen::Vector3d B = vertices_.row(faces_(face_id, 1));
  Eigen::Vector3d C = vertices_.row(faces_(face_id, 2));
  double a_squared = (B - C).squaredNorm();
  double b_squared = (C - A).squaredNorm();
  double c_squared = (A - B).squaredNorm();
  // Compute the area of the triangle. area = 1/2bcsinA.
  double area = (B - A).cross(C - A).norm() / 2;
  // Compute cotA = cosA / sinA.
  // b^2 + c^2 -2bccosA = a^2, or cosA = (b^2 + c^2 - a^2) / 2bc.
  // 1/2bcsinA = area, or sinA = 2area/bc.
  // cotA = (b^2 + c^2 -a^2) / 4area.
  double four_area = 4 * area;
  cotangent(0) = (b_squared + c_squared - a_squared) / four_area;
  cotangent(1) = (c_squared + a_squared - b_squared) / four_area;
  cotangent(2) = (a_squared + b_squared - c_squared) / four_area;
  return cotangent;
}

Energy AdmmFixedSolver::ComputeEnergy() const {
  // Compute the energy.
  Energy energy;
  // In order to do early return, let's first test all the S_ matrices to see
  // whether they belong to SO(3).
  double infty = std::numeric_limits<double>::infinity();
  int vertex_num = vertices_.rows();
  for (int i = 0; i < vertex_num; ++i) {
    if (!IsSO3(S_[i])) {
      std::cout << "This should never happen!" << std::endl;
      energy.AddEnergyType("Total", infty);
      return energy;
    }
  }

  // Now it passes the indicator function, the energy should be finite.
  int edge_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  double total = 0.0;
  int face_num = faces_.rows();
  for (int f = 0; f < face_num; ++f) {
    // Loop over all the edges.
    for (int e = 0; e < 3; ++e) {
      int first = faces_(f, edge_map[e][0]);
      int second = faces_(f, edge_map[e][1]);
      double edge_energy = 0.0;
      double weight = weight_.coeff(first, second);
      Eigen::Vector3d vec = (vertices_updated_.row(first) -
          vertices_updated_.row(second)).transpose() -
          rotations_[first] * (vertices_.row(first) -
          vertices_.row(second)).transpose();
      edge_energy = weight * vec.squaredNorm();
      total += edge_energy;
    }
  }
  energy.AddEnergyType("ARAP", total);

  // Add augmented term.
  double half_rho = rho_ / 2;
  double rotation_aug_energy = 0.0;
  for (int i = 0; i < vertex_num; ++i) {
    rotation_aug_energy += (rotations_[i] - S_[i]).squaredNorm();
  }
  rotation_aug_energy *= half_rho;
  total += rotation_aug_energy;
  energy.AddEnergyType("Rotation", rotation_aug_energy);
  energy.AddEnergyType("Total", total);
  return energy;
}

bool AdmmFixedSolver::CheckLinearSolve() const {
  // Compute the linear solve energy.
  // Don't pollute the solution! Play with a copy instead.
  Eigen::MatrixXd vertices = vertices_updated_;
  std::vector<Eigen::Matrix3d> R = rotations_;
  double optimal_energy = ComputeLinearSolveEnergy(vertices, R);
  std::cout << "Optimal linear energy: " << optimal_energy << std::endl;
  std::cout.precision(15);
  int free_num = free_.size();
  int cols = vertices.cols();
  double delta = 0.001;
  for (int i = 0; i < free_num; ++i) {
    for (int j = 0; j < cols; ++j) {
      int pos = free_(i);
      vertices(pos, j) += delta;
      double perturbed_enrgy = ComputeLinearSolveEnergy(vertices, R);
      double product = perturbed_enrgy - optimal_energy;
      // Reset value.
      vertices(pos, j) = vertices_updated_(pos, j);
      // Perturb in another direction.
      vertices(pos, j) -= delta;
      perturbed_enrgy = ComputeLinearSolveEnergy(vertices, R);
      product *= (perturbed_enrgy - optimal_energy);
      if (product < 0.0) {
        std::cout << "Linear solve check failed!" << std::endl;
        std::cout << "Error occurs in (" << pos << ", " << j << ")" << std::endl;
        return false;
      }
      // Reset value.
      vertices(pos, j) = vertices_updated_(pos, j);
    }
  }
  // Perturb the rotations.
  int vertex_num = vertices_.rows();
  for (int v = 0; v < vertex_num; ++v) {
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        // Perturb R[v](i, j).
        R[v](i, j) += delta;
        double perturbed_enrgy = ComputeLinearSolveEnergy(vertices, R);
        double product = perturbed_enrgy - optimal_energy;
        R[v](i, j) = rotations_[v](i, j);
        R[v](i, j) -= delta;
        perturbed_enrgy = ComputeLinearSolveEnergy(vertices, R);
        product *= (perturbed_enrgy - optimal_energy);
        if (product < 0.0) {
          std::cout << "Linear solve check failed!" << std::endl;
          std::cout << "Error occurs in (" << v << ", " << i << ", " << j << ")" << std::endl;
          // Print out the parabola.
          std::cout << "Variable\tEnergy\n";
          for (int step = -10; step < 10; ++step) {
            R[v](i, j) = rotations_[v](i, j) + step * delta;
            std::cout << R[v](i, j) << "\t" << ComputeLinearSolveEnergy(vertices, R)
              << std::endl;
          }
          return false;
        }
        R[v](i, j) = rotations_[v](i, j);
      }
    }
  }
  std::cout << "All linear solve tests passed!" << std::endl;
  return true;
}

double AdmmFixedSolver::ComputeLinearSolveEnergy(const Eigen::MatrixXd &vertices,
    const std::vector<Eigen::Matrix3d> &rotations) const {
  // Compute the linear solve energy.
  double energy = 0.0;
  int edge_map[3][2] = { {1, 2}, {2, 0}, {0, 1} };
  int face_num = faces_.rows();
  int vertex_num = vertices_.rows();
  for (int f = 0; f < face_num; ++f) {
    // Loop over all the edges.
    for (int e = 0; e < 3; ++e) {
      int first = faces_(f, edge_map[e][0]);
      int second = faces_(f, edge_map[e][1]);
      double edge_energy = 0.0;
      double weight = weight_.coeff(first, second);
      Eigen::Vector3d vec = (vertices.row(first) -
          vertices.row(second)).transpose() -
          rotations[first] * (vertices_.row(first) -
          vertices_.row(second)).transpose();
      edge_energy = weight * vec.squaredNorm();
      energy += edge_energy;
    }
  }
  // Add up the augmented rotation term.
  double weight = rho_ / 2;
  for (int v = 0; v < vertex_num; ++v) {
    energy += weight * (rotations[v] - S_[v] + T_[v]).squaredNorm();
  }
  return energy;
}

// Compute the SVD solve energy. Used in CheckSVDSolve.
double AdmmFixedSolver::ComputeSVDSolveEnergy() const {
  double infty = std::numeric_limits<double>::infinity();
  int vertex_num = vertices_.rows();
  for (int v = 0; v < vertex_num; ++v) {
    if (!IsSO3(S_[v])) {
      return infty;
    }
  }
  double energy = 0.0;
  for (int v = 0; v < vertex_num; ++v) {
    energy += (rotations_[v] - S_[v] + T_[v]).squaredNorm();
  }
  energy *= rho_ / 2;
  return energy;
}

// Check whether a matrix is in SO(3).
bool AdmmFixedSolver::IsSO3(const Eigen::Matrix3d &S) const {
  double det = S.determinant();
  if ((S * S.transpose() - Eigen::Matrix3d::Identity()).squaredNorm()
      > kMatrixDiffThreshold || abs(det - 1) > kMatrixDiffThreshold) {
    std::cout << "S does not belong to SO(3)" << std::endl;
    std::cout << "S: \n" << S << std::endl;
    return false;
  }
  return true;
}

}  // namespace demo
}  // namespace arap