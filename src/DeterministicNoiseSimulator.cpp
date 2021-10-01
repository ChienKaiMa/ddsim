#include "DeterministicNoiseSimulator.hpp"

#include "dd/Export.hpp"

using CN    = dd::ComplexNumbers;
using dEdge = dd::Package::dEdge;
using Edge  = dd::Edge<dd::Package::dNode>;

dEdge DeterministicNoiseSimulator::makeZeroDensityOperator(dd::QubitCount n) {
    auto f = dEdge::one;
    assert((signed char)n == n);
    for (dd::Qubit p = 0; p < (signed char)n; p++) {
        f = dd->makeDDNode(p, std::array{f, dEdge::zero, dEdge::zero, dEdge::zero});
    }
    return f;
}

dd::fp DeterministicNoiseSimulator::probForIndexToBeZero(dEdge e, dd::Qubit index, dd::fp pathProb, dd::fp global_prob) {
    if (Edge::isDensityMatrix(e.p)) {
        dEdge newEdge{dd->dUniqueTable.getNode(), e.w};
        e = Edge::getAlignedDensityNodeCopy(e, newEdge);
    }

    if (e.w.approximatelyZero() || (e.p->v == index && e.p->e[0].w.approximatelyZero())) {
        return 0;
    }

    assert(!e.isTerminal());
    assert(dd::CTEntry::val(e.w.i) == 0);
    pathProb = pathProb * dd::CTEntry::val(e.w.r);

    if (e.p->v > index) {
        global_prob += probForIndexToBeZero(e.p->e[0], index, pathProb, global_prob);
        global_prob += probForIndexToBeZero(e.p->e[3], index, pathProb, global_prob);
        return global_prob;
    } else {
        return pathProb;
    }
}

char DeterministicNoiseSimulator::MeasureOneCollapsing(dd::Qubit index) {
    char                                   result   = 'n';
    auto                                   f        = qc::MatrixDD::one;
    auto                                   n_qubits = getNumberOfQubits();
    std::uniform_real_distribution<dd::fp> dist(0.0, 1.0L);
    dd::fp                                 n = dist(mt);

    auto prob_zero = probForIndexToBeZero(density_root_edge, index, 1, 0);

    for (dd::Qubit p = 0; p < (signed char)n_qubits; p++) {
        if (p == index) {
            if (prob_zero >= n) {
                // Build the operation that it sets index to 0
                result = '0';
                f      = dd->makeDDNode(static_cast<dd::Qubit>(p), std::array{f, qc::MatrixDD::zero, qc::MatrixDD::zero, qc::MatrixDD::zero});
            } else {
                result = '1';
                // Build the operation that it sets index to 1
                f = dd->makeDDNode(static_cast<dd::Qubit>(p), std::array{qc::MatrixDD::zero, qc::MatrixDD::zero, qc::MatrixDD::zero, f});
            }
        } else {
            f = dd->makeDDNode(static_cast<dd::Qubit>(p), std::array{f, qc::MatrixDD::zero, qc::MatrixDD::zero, f});
        }
    }
    //    qc::MatrixDD tmp0 = dd->multiply(dd->multiply(f, density_root_edge), dd->conjugateTranspose(f));
    auto tmp0 = dd->conjugateTranspose(f);
    auto tmp1 = dd->multiply(density_root_edge, reinterpret_cast<dEdge&>(tmp0), 0, false);
    auto tmp2 = dd->multiply(reinterpret_cast<dEdge&>(f), tmp1, 0, true);

    dd->incRef(tmp0);
    dd->decRef(density_root_edge);
    density_root_edge = reinterpret_cast<dEdge&>(tmp0);

    // Normalize the density matrix
    auto trace = dd->trace(reinterpret_cast<dd::Package::mEdge&>(density_root_edge));
    assert(trace.i == 0);
    auto tmp_complex = dd->cn.getTemporary(1 / trace.r, 0);
    dd::ComplexNumbers::mul(tmp_complex, density_root_edge.w, tmp_complex);
    dd::ComplexNumbers::decRef(density_root_edge.w);
    density_root_edge.w = dd->cn.lookup(tmp_complex);
    return result;
}

std::map<std::string, double> DeterministicNoiseSimulator::DeterministicSimulate() {
    const unsigned short         n_qubits = qc->getNqubits();
    std::map<unsigned int, bool> classic_values;
    int                          opCount = 0;

    density_root_edge = makeZeroDensityOperator(n_qubits);

    dd->incRef(density_root_edge);

    for (auto const& op: *qc) {
//        dd->garbageCollect(true); //todo remove the true flag after debugging
        dd->garbageCollect();
        opCount++;
        if (!op->isUnitary() && !(op->isClassicControlledOperation())) {
            if (auto* nu_op = dynamic_cast<qc::NonUnitaryOperation*>(op.get())) {
                if (op->getType() == qc::Measure) {
                    auto quantum = nu_op->getTargets();
                    auto classic = nu_op->getClassics();

                    if (quantum.size() != classic.size()) {
                        throw std::runtime_error("Measurement: Sizes of quantum and classic register mismatch.");
                    }

                    for (unsigned int i = 0; i < quantum.size(); ++i) {
                        auto result = MeasureOneCollapsing(quantum.at(i));
                        assert(result == '0' || result == '1');
                        classic_values[classic.at(i)] = (result == '1');
                    }
                } else if (op->getType() == qc::Reset) {
                    //                    // Reset qubit
                    throw std::runtime_error(std::string{"Unsupported non-unitary functionality: \""} + nu_op->getName() + "\"");
                } else {
                    //Skipping barrier
                    if (op->getType() == qc::Barrier) {
                        continue;
                    }
                    throw std::runtime_error(std::string{"Unsupported non-unitary functionality: \""} + nu_op->getName() + "\"");
                }
            } else {
                throw std::runtime_error("Dynamic cast to NonUnitaryOperation failed.");
            }
            dd->garbageCollect();
        } else {
            qc::MatrixDD dd_op = {};
            qc::Targets  targets;
            dd::Controls controls;
            if (op->isClassicControlledOperation()) {
                // Check if the operation is controlled by a classical register
                auto* classic_op    = dynamic_cast<qc::ClassicControlledOperation*>(op.get());
                bool  conditionTrue = true;
                auto  expValue      = classic_op->getExpectedValue();

                for (unsigned int i = classic_op->getControlRegister().first;
                     i < classic_op->getControlRegister().second; i++) {
                    if (classic_values[i] != (expValue % 2)) {
                        conditionTrue = false;
                        break;
                    } else {
                        expValue = expValue >> 1u;
                    }
                }
                dd_op   = classic_op->getOperation()->getDD(dd);
                targets = classic_op->getOperation()->getTargets();
                assert(targets.size() == 1);
                controls = classic_op->getOperation()->getControls();
                if (!conditionTrue) {
                    continue;
                }
            } else {
                dd_op   = op->getDD(dd);
                targets = op->getTargets();
                assert(targets.size() == 1);
                controls = op->getControls();
            }
//            printf("\n\nPrior op\n");
//            dd->printMatrix(reinterpret_cast<dd::Package::mEdge&>(density_root_edge));
//            //                        density_root_edge.p = Edge::getAlignedDensityNode(density_root_edge.p);
//            //                        printf("Matrix: (%.5lf + %.5lfI) * (%.5lf + %.5lfI) * (%.5lf + %.5lfI) \n",
//            //                               dd::CTEntry::val(density_root_edge.w.r), dd::CTEntry::val(density_root_edge.w.i),
//            //                               dd::CTEntry::val(density_root_edge.p->e[1].w.r), dd::CTEntry::val(density_root_edge.p->e[1].w.i),
//            //                               dd::CTEntry::val(density_root_edge.p->e[1].p->e[2].w.r), dd::CTEntry::val(density_root_edge.p->e[1].p->e[2].w.i));
//            //                        density_root_edge.p = Edge::setDensityMatrixTrue(density_root_edge.p);
//
//            printf("\n\nOp\n");
//            dd->printMatrix(dd_op);

            // Applying the operation to the density matrix
            auto tmp0 = dd->conjugateTranspose(dd_op);
            auto tmp1 = dd->multiply(density_root_edge, reinterpret_cast<dEdge&>(tmp0), 0, false);

//            printf("\n\nMid operation\n");
//            dd->printMatrix(reinterpret_cast<dd::Package::mEdge&>(tmp1));
//
//            //            dd::export2Dot(reinterpret_cast<dd::Package::mEdge&>(tmp1), "/home/toor/dds/ddMidOp.dot", false, true, false, false);
//            //            dd::export2Dot(reinterpret_cast<dd::Package::mEdge&>(dd_op), "/home/toor/dds/dd_op.dot", false, true, false, false);
//            //
//            printf("Matrix: (%.5lf + %.5lfI) * (%.5lf + %.5lfI) * (%.5lf + %.5lfI) \n",
//                   dd::CTEntry::val(tmp1.w.r), dd::CTEntry::val(tmp1.w.i),
//                   dd::CTEntry::val(tmp1.p->e[3].w.r), dd::CTEntry::val(tmp1.p->e[3].w.i),
//                   dd::CTEntry::val(tmp1.p->e[3].p->e[2].w.r), dd::CTEntry::val(tmp1.p->e[3].p->e[2].w.i));
            //
            //            printf("Operation: (%.5lf + %.5lfI) * (%.5lf + %.5lfI) * (%.5lf + %.5lfI) \n", dd::CTEntry::val(dd_op.w.r), dd::CTEntry::val(dd_op.w.i),
            //                   dd::CTEntry::val(dd_op.p->e[0].w.r), dd::CTEntry::val(dd_op.p->e[0].w.i),
            //                   dd::CTEntry::val(dd_op.p->e[0].p->e[3].w.r), dd::CTEntry::val(dd_op.p->e[0].p->e[3].w.i));

            auto tmp2 = dd->multiply(reinterpret_cast<dEdge&>(dd_op), tmp1, 0, use_density_matrix_type);
            dd->incRef(tmp2);
            density_root_edge.p = Edge::getAlignedDensityNode(density_root_edge.p);
            dd->decRef(density_root_edge);
            density_root_edge = tmp2;

            if (use_density_matrix_type) {
                density_root_edge.p = Edge::setDensityMatrixTrue(density_root_edge.p);
            }

            //            printf("\n\nAfter op\n");
            //            dd->printMatrix(reinterpret_cast<dd::Package::mEdge&>(density_root_edge));
            //            dd->printMatrix(dd_op);
            //            dd::export2Dot(reinterpret_cast<dd::Package::mEdge&>(tmp2), "/home/toor/dds/ddAfterOp.dot", false, true, false, false);
            //
            //            printf("Matrix: (%.5lf + %.5lfI) * (%.5lf + %.5lfI) * (%.5lf + %.5lfI) \n", dd::CTEntry::val(tmp1.w.r), dd::CTEntry::val(tmp1.w.i),
            //                   dd::CTEntry::val(tmp1.p->e[0].w.r), dd::CTEntry::val(tmp1.p->e[0].w.i),
            //                   dd::CTEntry::val(tmp1.p->e[0].p->e[1].w.r), dd::CTEntry::val(tmp1.p->e[0].p->e[1].w.i));

            if (noiseProbability > 0) {
                if (sequentialApplyNoise) { // was `stochastic_runs == -2`
                    [[maybe_unused]] auto cache_size_before = dd->cn.cacheCount();

                    auto seq_targets = targets;
                    for (auto const& control: controls) {
                        seq_targets.push_back(control.qubit);
                    }
                    applyDetNoiseSequential(seq_targets);

                    [[maybe_unused]] auto cache_size_after = dd->cn.cacheCount();
                    assert(cache_size_after == cache_size_before);
                } else {
                    signed char maxDepth       = targets[0];
                    auto        control_qubits = op->getControls();
                    for (auto& control: control_qubits) {
                        if (control.qubit < maxDepth) {
                            maxDepth = control.qubit;
                        }
                    }
                    [[maybe_unused]] auto cache_size_before = dd->cn.cacheCount();
                    auto                  tmp3              = ApplyNoiseEffects(density_root_edge, op, 0);
                    if (!tmp3.w.approximatelyZero()) {
                        //                    if (!(tmp3.w != dd::Complex::zero)) {
                        dd::Complex c = dd->cn.lookup(tmp3.w);
                        dd->cn.returnToCache(tmp3.w);
                        tmp3.w = c;
                    }

                    [[maybe_unused]] auto cache_size_after = dd->cn.cacheCount();
                    assert(cache_size_after == cache_size_before);

                    dd->incRef(tmp3);

                    density_root_edge.p = Edge::getAlignedDensityNode(density_root_edge.p);
                    dd->decRef(density_root_edge);
                    density_root_edge = tmp3;
                    if (use_density_matrix_type) {
                        density_root_edge.p = Edge::setDensityMatrixTrue(density_root_edge.p);
                    }
                }
                //                printf("\n\nAfter Noise op\n");
                //                dd->printMatrix(reinterpret_cast<dd::Package::mEdge&>(density_root_edge));
            }
        }
    }
    dd->garbageCollect(true);
    //    density_root_edge.p = Edge::setDensityMatrixTrue(density_root_edge.p);
    //    if (!use_density_matrix_type) {
    //        dd::export2Dot(reinterpret_cast<dd::Package::mEdge&>(density_root_edge), "/home/toor/dds/ddFinal.dot", false, true, false, false);
    //    }

//    printf("\n\nFinal State\n");
//    dd->printMatrix(reinterpret_cast<dd::Package::mEdge&>(density_root_edge));
//    density_root_edge.p = Edge::getAlignedDensityNode(density_root_edge.p);
//    printf("Matrix: (%.5lf + %.5lfI) * (%.5lf + %.5lfI) * (%.5lf + %.5lfI) \n",
//           dd::CTEntry::val(density_root_edge.w.r), dd::CTEntry::val(density_root_edge.w.i),
//           dd::CTEntry::val(density_root_edge.p->e[3].w.r), dd::CTEntry::val(density_root_edge.p->e[3].w.i),
//           dd::CTEntry::val(density_root_edge.p->e[3].p->e[2].w.r), dd::CTEntry::val(density_root_edge.p->e[3].p->e[2].w.i));
//    dd::export2Dot(reinterpret_cast<dd::Package::mEdge&>(density_root_edge), "/home/toor/dds/finalOhneCache.dot", false, true, false, false);
//    density_root_edge.p = Edge::setDensityMatrixTrue(density_root_edge.p);

    //    dd->printMhtatrix(density_root_edge, true);
    //    printf("Final result\n");
    //    dd->printMatrix(reinterpret_cast<dd::Package::mEdge&>(density_root_edge));
    //    dd->printMatrix(density_root_edge, false);
    //    dd->densityNoiseOperations.printStatistics();

//    dd->mUniqueTable.printStatistics();
//    dd->dUniqueTable.printStatistics();
    dd->matrixDensityMultiplication.printStatistics();
    return AnalyseState(n_qubits, false);
    //    return {};
}

dEdge DeterministicNoiseSimulator::ApplyNoiseEffects(dEdge& originalEdge, const std::unique_ptr<qc::Operation>& op, unsigned char maxDepth) {
    auto alignedEdge = originalEdge;

    if (Edge::isDensityMatrix(originalEdge.p)) {
        dEdge newEdge{dd->dUniqueTable.getNode(), alignedEdge.w};
        alignedEdge = Edge::getAlignedDensityNodeCopy(alignedEdge, newEdge);
    }

    if (alignedEdge.p->v < maxDepth || alignedEdge.isTerminal()) {
        dEdge tmp{};
        if (!alignedEdge.w.approximatelyZero()) {
            tmp.w = dd->cn.getCached(dd::CTEntry::val(alignedEdge.w.r), dd::CTEntry::val(alignedEdge.w.i));
        } else {
            tmp.w = dd::Complex::zero;
        }
        if (alignedEdge.isTerminal()) {
            return dEdge::terminal(tmp.w);
        } else {
            tmp.p = alignedEdge.p;
            return tmp;
        }
    }

    std::vector used_qubits = op->getTargets();
    for (auto control: op->getControls()) {
        used_qubits.push_back(control.qubit);
    }
    sort(used_qubits.begin(), used_qubits.end());

    std::array<dEdge, 4> new_edges{};
    for (int i = 0; i < 4; i++) {
        //        if (alignedEdge.p->e[i].w.approximatelyZero()) {
        //            new_edges[i] = dEdge::terminal(alignedEdge.p->e[i].w);
        //            continue;
        //        }

        // Check if the target of the current edge is in the Compute table. Note that I check for the target node of
        // the current edge if noise needs to be applied or not

        auto tmp = dd->densityNoiseOperations.lookup(alignedEdge.p->e[i], dd->cn.getTemporary(), used_qubits);
        //        qc::MatrixDD tmp = noiseLookup(alignedEdge.p->e[i], used_qubits);
        if (tmp.p != nullptr) {
            if (tmp.w.approximatelyZero()) {
                tmp = dEdge::zero;
            } else {
                tmp.w = dd->cn.getCached(tmp.w.r->value, tmp.w.i->value);
            }
            new_edges[i] = tmp;
            continue;
        }

        new_edges[i] = ApplyNoiseEffects(alignedEdge.p->e[i], op, maxDepth);
        //        new_edges[i] = ApplyNoiseEffects(dd->getAlignedDensityNodeCopy(&alignedEdge.p->e[i]), op, maxDepth);

        // Adding the operation to the operation table
        dd->densityNoiseOperations.insert(alignedEdge.p->e[i], new_edges[i], used_qubits);
        //        noiseInsert(alignedEdge.p->e[i], used_qubits, new_edges[i]);
    }

    if (op->actsOn(alignedEdge.p->v)) {
        for (auto const& type: gateNoiseTypes) {
            switch (type) {
                case 'A':
                    ApplyAmplitudeDampingToNode(new_edges);
                    break;
                case 'P':
                    ApplyPhaseFlipToNode(new_edges);
                    break;
                case 'D':
                    ApplyDepolaritationToNode(new_edges);
                    break;
                default:
                    throw std::runtime_error(std::string("Unknown gate noise type '") + type + "'");
            }
        }
    }

    if (use_density_matrix_type && !Edge::isFirstEdgeDensityPath(originalEdge.p)) {
        new_edges[2].p          = new_edges[1].p;
        new_edges[2].w.r->value = dd::CTEntry::val(new_edges[1].w.r);
        new_edges[2].w.i->value = dd::CTEntry::val(new_edges[1].w.i);
    }

    //    qc::MatrixDD tmp = dd->makeDDNode(alignedEdge.p->v, new_edges, true, true);
    auto tmp = dd->makeDDNode(alignedEdge.p->v, new_edges, true);

    // Multiplying the old edge weight with the new one
    if (!tmp.w.approximatelyZero()) {
        CN::mul(tmp.w, tmp.w, alignedEdge.w);
    }
    return tmp;
}

void DeterministicNoiseSimulator::ApplyPhaseFlipToNode(std::array<dEdge, 4>& e) {
    double      probability  = noiseProbability;
    dd::Complex complex_prob = dd->cn.getCached();

    //e[0] = e[0]

    //e[1] = (1-2p)*e[1]
    if (!e[1].w.approximatelyZero()) {
        complex_prob.r->value = 1 - 2 * probability;
        complex_prob.i->value = 0;
        CN::mul(e[1].w, complex_prob, e[1].w);
    }

    //e[2] = (1-2p)*e[2]
    if (!e[2].w.approximatelyZero()) {
        if (e[1].w.approximatelyZero()) {
            complex_prob.r->value = 1 - 2 * probability;
            complex_prob.i->value = 0;
        }
        CN::mul(e[2].w, complex_prob, e[2].w);
    }

    //e[3] = e[3]

    dd->cn.returnToCache(complex_prob);
}

void DeterministicNoiseSimulator::ApplyAmplitudeDampingToNode(std::array<dEdge, 4>& e) {
    double      probability  = noiseProbability * 2;
    dd::Complex complex_prob = dd->cn.getCached();
    dEdge       helper_edge[1];
    helper_edge[0].w = dd->cn.getCached();

    // e[0] = e[0] + p*e[3]
    if (!e[3].w.approximatelyZero()) {
        complex_prob.r->value = probability;
        complex_prob.i->value = 0;
        if (!e[0].w.approximatelyZero()) {
            CN::mul(helper_edge[0].w, complex_prob, e[3].w);
            helper_edge[0].p = e[3].p;
            dd::Edge tmp     = dd->add2(e[0], helper_edge[0]);
            if (!e[0].w.approximatelyZero()) {
                dd->cn.returnToCache(e[0].w);
            }
            e[0] = tmp;
        } else {
            e[0].w = dd->cn.getCached();
            CN::mul(e[0].w, complex_prob, e[3].w);
            e[0].p = e[3].p;
        }
    }

    //e[1] = sqrt(1-p)*e[1]
    if (!e[1].w.approximatelyZero()) {
        complex_prob.r->value = std::sqrt(1 - probability);
        complex_prob.i->value = 0;
        CN::mul(e[1].w, complex_prob, e[1].w);
    }

    //e[2] = sqrt(1-p)*e[2]
    if (!e[2].w.approximatelyZero()) {
        if (e[1].w.approximatelyZero()) {
            complex_prob.r->value = std::sqrt(1 - probability);
            complex_prob.i->value = 0;
        }
        CN::mul(e[2].w, complex_prob, e[2].w);
    }

    //e[3] = (1-p)*e[3]
    if (!e[3].w.approximatelyZero()) {
        complex_prob.r->value = 1 - probability;
        CN::mul(e[3].w, complex_prob, e[3].w);
    }

    dd->cn.returnToCache(helper_edge[0].w);
    dd->cn.returnToCache(complex_prob);
}

void DeterministicNoiseSimulator::ApplyDepolaritationToNode(std::array<dEdge, 4>& e) {
    double probability = noiseProbability;
    dEdge  helper_edge[2];
    helper_edge[0].w         = dd->cn.getCached();
    helper_edge[1].w         = dd->cn.getCached();
    dd::Complex complex_prob = dd->cn.getCached();

    //todo I don't have to save all edges
    dEdge old_edges[4];
    for (int i = 0; i < 4; i++) {
        if (!e[i].w.approximatelyZero()) {
            old_edges[i].w = dd->cn.getCached(dd::CTEntry::val(e[i].w.r), dd::CTEntry::val(e[i].w.i));
            old_edges[i].p = e[i].p;
        } else {
            old_edges[i] = e[i];
        }
    }

    //e[0] = 0.5*((2-p)*e[0] + p*e[3])
    complex_prob.i->value = 0;
    // first check if e[0] or e[1] != 0
    if (!old_edges[0].w.approximatelyZero() || !old_edges[3].w.approximatelyZero()) {
        if (!old_edges[0].w.approximatelyZero() && old_edges[3].w.approximatelyZero()) {
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(e[0].w, complex_prob, old_edges[0].w);
            e[0].p = old_edges[0].p;
        } else if (old_edges[0].w.approximatelyZero() && !old_edges[3].w.approximatelyZero()) {
            e[0].w                = dd->cn.getCached();
            complex_prob.r->value = probability * 0.5;
            CN::mul(e[0].w, complex_prob, old_edges[3].w);
            e[0].p = old_edges[3].p;
        } else {
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(helper_edge[0].w, complex_prob, old_edges[0].w);
            complex_prob.r->value = probability * 0.5;
            CN::mul(helper_edge[1].w, complex_prob, old_edges[3].w);
            helper_edge[0].p = old_edges[0].p;
            helper_edge[1].p = old_edges[3].p;
            dd->cn.returnToCache(e[0].w);
            e[0] = dd->add2(helper_edge[0], helper_edge[1]);
        }
    }
    //e[1]=1-p*e[1]
    if (!e[1].w.approximatelyZero()) {
        complex_prob.r->value = 1 - probability;
        CN::mul(e[1].w, e[1].w, complex_prob);
    }
    //e[2]=1-p*e[2]
    if (!e[2].w.approximatelyZero()) {
        if (e[1].w.approximatelyZero()) {
            complex_prob.r->value = 1 - probability;
        }
        CN::mul(e[2].w, e[2].w, complex_prob);
    }

    //e[3] = 0.5*((2-p)*e[3] + p*e[0])
    if (!old_edges[0].w.approximatelyZero() || !old_edges[3].w.approximatelyZero()) {
        if (!old_edges[0].w.approximatelyZero() && old_edges[3].w.approximatelyZero()) {
            e[3].w                = dd->cn.getCached();
            complex_prob.r->value = probability * 0.5;
            CN::mul(e[3].w, complex_prob, old_edges[0].w);
            e[3].p = old_edges[0].p;
        } else if (old_edges[0].w.approximatelyZero() && !old_edges[3].w.approximatelyZero()) {
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(e[3].w, complex_prob, old_edges[3].w);
            e[3].p = old_edges[3].p;
        } else {
            complex_prob.r->value = probability * 0.5;
            CN::mul(helper_edge[0].w, complex_prob, old_edges[0].w);
            complex_prob.r->value = (2 - probability) * 0.5;
            CN::mul(helper_edge[1].w, complex_prob, old_edges[3].w);
            helper_edge[0].p = old_edges[0].p;
            helper_edge[1].p = old_edges[3].p;
            dd->cn.returnToCache(e[3].w);
            e[3] = dd->add2(helper_edge[0], helper_edge[1]);
        }
    }
    for (auto& old_edge: old_edges) {
        if (!old_edge.w.approximatelyZero()) {
            dd->cn.returnToCache(old_edge.w);
        }
    }
    //    helper_edge[0].w.r->next->next->next->next->next->next = ComplexCache_Avail;
    dd->cn.returnToCache(helper_edge[0].w);
    dd->cn.returnToCache(helper_edge[1].w);
    dd->cn.returnToCache(complex_prob);
}

std::map<std::string, double> DeterministicNoiseSimulator::AnalyseState(dd::QubitCount nr_qubits, bool full_state) {
    std::map<std::string, double> measuredResult = {};
    double                        p0, p1;
    double long                   global_probability;
    double                        statesToMeasure;

    density_root_edge.p = Edge::getAlignedDensityNode(density_root_edge.p);

    if (full_state) {
        statesToMeasure = pow(2, nr_qubits);
    } else {
        statesToMeasure = std::min((double)256, pow(2, nr_qubits));
    }

    for (int m = 0; m < statesToMeasure; m++) {
        int current_result         = m;
        global_probability         = dd::CTEntry::val(density_root_edge.w.r);
        std::string  result_string = intToString(m, '1');
        qc::MatrixDD cur           = reinterpret_cast<const dd::Package::mEdge&>(density_root_edge);
        for (int i = 0; i < nr_qubits; ++i) {
            if (cur.p->v != -1) {
                assert(dd::CTEntry::val(cur.p->e.at(0).w.i) + dd::CTEntry::val(cur.p->e[3].w.i) == 0);
                p0 = dd::CTEntry::val(cur.p->e.at(0).w.r);
                p1 = dd::CTEntry::val(cur.p->e.at(3).w.r);
            } else {
                global_probability = 0;
                break;
            }

            if (current_result % 2 == 0) {
                cur = cur.p->e.at(0);
                global_probability *= p0;
            } else {
                cur = cur.p->e.at(3);
                global_probability *= p1;
            }
            current_result = current_result >> 1;
        }
        if (global_probability > 0.01) {
            measuredResult.insert({result_string, global_probability});
        }
    }
    return measuredResult;
}

void DeterministicNoiseSimulator::generateGate(qc::MatrixDD* pointer_for_matrices, char noise_type, dd::Qubit target) {
    std::array<dd::GateMatrix, 4> idle_noise_gate{};
    dd::ComplexValue              tmp = {};

    double probability = noiseProbability;

    switch (noise_type) {
        // bitflip
        //      (sqrt(1-probability)    0           )       (0      sqrt(probability))
        //  e0= (0            sqrt(1-probability)   ), e1=  (sqrt(probability)      0)
        //        case 'B': {
        //            tmp.r                 = std::sqrt(1 - probability) * dd::complex_one.r;
        //            idle_noise_gate[0][0] = idle_noise_gate[0][3] = tmp;
        //            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;
        //            tmp.r                                         = std::sqrt(probability) * dd::complex_one.r;
        //            idle_noise_gate[1][1] = idle_noise_gate[1][2] = tmp;
        //            idle_noise_gate[1][0] = idle_noise_gate[1][3] = dd::complex_zero;
        //
        //            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);
        //            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);
        //            break;
        //        }
        // phase flip
        //      (sqrt(1-probability)    0           )       (sqrt(probability)      0)
        //  e0= (0            sqrt(1-probability)   ), e1=  (0      -sqrt(probability))
        case 'P': {
            tmp.r                 = std::sqrt(1 - probability) * dd::complex_one.r;
            idle_noise_gate[0][0] = idle_noise_gate[0][3] = tmp;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;
            tmp.r                                         = std::sqrt(probability) * dd::complex_one.r;
            idle_noise_gate[1][0]                         = tmp;
            tmp.r *= -1;
            idle_noise_gate[1][3] = tmp;
            idle_noise_gate[1][1] = idle_noise_gate[1][2] = dd::complex_zero;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);
            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);

            break;
        }
            // amplitude damping
            //      (1      0           )       (0      sqrt(probability))
            //  e0= (0      sqrt(1-probability)   ), e1=  (0      0      )
        case 'A': {
            tmp.r                 = std::sqrt(1 - probability * 2) * dd::complex_one.r;
            idle_noise_gate[0][0] = dd::complex_one;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;
            idle_noise_gate[0][3]                         = tmp;

            tmp.r                 = std::sqrt(probability * 2) * dd::complex_one.r;
            idle_noise_gate[1][0] = idle_noise_gate[1][3] = idle_noise_gate[1][2] = dd::complex_zero;
            idle_noise_gate[1][1]                                                 = tmp;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);
            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);
            break;
        }
            // depolarization
        case 'D': {
            tmp.r = std::sqrt(1 - ((3 * probability) / 4)) * dd::complex_one.r;
            //                   (1 0)
            // sqrt(1- ((3p)/4))*(0 1)
            idle_noise_gate[0][0] = idle_noise_gate[0][3] = tmp;
            idle_noise_gate[0][1] = idle_noise_gate[0][2] = dd::complex_zero;

            pointer_for_matrices[0] = dd->makeGateDD(idle_noise_gate[0], getNumberOfQubits(), target);

            //            (0 1)
            // sqrt(probability/4))*(1 0)
            tmp.r                 = std::sqrt(probability / 4) * dd::complex_one.r;
            idle_noise_gate[1][1] = idle_noise_gate[1][2] = tmp;
            idle_noise_gate[1][0] = idle_noise_gate[1][3] = dd::complex_zero;

            pointer_for_matrices[1] = dd->makeGateDD(idle_noise_gate[1], getNumberOfQubits(), target);

            //            (1 0)
            // sqrt(probability/4))*(0 -1)
            tmp.r                 = std::sqrt(probability / 4) * dd::complex_one.r;
            idle_noise_gate[2][0] = tmp;
            tmp.r                 = tmp.r * -1;
            idle_noise_gate[2][3] = tmp;
            idle_noise_gate[2][1] = idle_noise_gate[2][2] = dd::complex_zero;

            pointer_for_matrices[3] = dd->makeGateDD(idle_noise_gate[2], getNumberOfQubits(), target);

            //            (0 -i)
            // sqrt(probability/4))*(i 0)
            tmp.r                 = dd::complex_zero.r;
            tmp.i                 = std::sqrt(probability / 4) * 1;
            idle_noise_gate[3][2] = tmp;
            tmp.i                 = tmp.i * -1;
            idle_noise_gate[3][1] = tmp;
            idle_noise_gate[3][0] = idle_noise_gate[3][3] = dd::complex_zero;

            pointer_for_matrices[2] = dd->makeGateDD(idle_noise_gate[3], getNumberOfQubits(), target);
            break;
        }
        default:
            throw std::runtime_error("Unknown noise effect received.");
    }
}

void DeterministicNoiseSimulator::applyDetNoiseSequential(const qc::Targets& targets) {
    //    qc::MatrixDD tmp = {};
    //    //    qc::MatrixDD ancillary_edge_1 = {};
    //    qc::MatrixDD idle_operation[4];
    //
    //    // Iterate over qubits and check if the qubit had been used
    //    for (auto target_qubit: targets) {
    //        for (auto const& type: gateNoiseTypes) {
    //            generateGate(idle_operation, type, target_qubit);
    //            tmp.p = nullptr;
    //            //Apply all noise matrices of the current noise effect
    //            for (int m = 0; m < noiseEffects.find(type)->second; m++) {
    //                auto tmp0 = dd->multiply(dd->multiply(idle_operation[m], density_root_edge), dd->conjugateTranspose(idle_operation[m]));
    //                if (tmp.p == nullptr) {
    //                    tmp = tmp0;
    //                } else {
    //                    tmp = dd->add(tmp0, tmp);
    //                }
    //            }
    //            dd->incRef(tmp);
    //            dd->decRef(density_root_edge);
    //            density_root_edge = tmp;
    //        }
    //    }
}

std::string DeterministicNoiseSimulator::intToString(long target_number, char value) const {
    if (target_number < 0) {
        assert(target_number == -1);
        return (std::string("F"));
    }
    auto        qubits = getNumberOfQubits();
    std::string path(qubits, '0');
    auto        number = (unsigned long)target_number;
    for (int i = 1; i <= qubits; i++) {
        if (number % 2) {
            path[qubits - i] = value;
        }
        number = number >> 1u;
    }
    return path;
}