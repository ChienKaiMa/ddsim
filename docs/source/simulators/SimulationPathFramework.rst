Simulation Path Framework
=========================

The tool also provides a framework for exploiting arbitrary simulation paths (using the `taskflow <https://github.com/taskflow/taskflow>`_ library) based on the methods proposed in :cite:p:`burgholzer2021hybrid`.
A *simulation path* describes the order in which the individual MxV or MxM multiplications are conducted during the simulation of a quantum circuit.
It can be described as a list of tuples that identify the individual states/operations to be multiplied.
The initial state is always assigned :code:`ID 0`, while the i-th operation is assigned :code:`ID i`.
The result of a multiplication is assigned the next highest, unused ID, e.g., in a circuit with :code:`n` gates the result of the first multiplication is assigned :code:`ID n+1`.

The framework comes with several pre-defined simulation path strategies:

 - :code:`sequential` (*default*): simulate the circuit sequentially from left to right using only MxV multiplications
 - :code:`pairwise_recursive`: recursively group pairs of states and operations to form a binary tree of MxV/MxM multiplications
 - :code:`bracket`: group certain number of operations according to a given :code:`bracket_size`
 - :code:`alternating`: start the simulation in the middle of the circuit and alternate between applications of gates "from the left" and "from the right" (which might potentially be useful for equivalence checking)

 as well as the option to translate strategies from the domain of tensor networks to decision diagrams (using the `CoTenGra <https://github.com/jcmgray/cotengra>`_ library), see [here](#cotengra).

Basic Example
#############

This example shall serve as a showcase on how to use the simulation path framework (via Python).
First, create the circuit to be simulated using qiskit, e.g., in this case a three-qubit GHZ state:

.. code-block:: python3

    from qiskit import *

    circ = QuantumCircuit(3)
    # the initial state corresponds to ID 0
    circ.h(0)         # corresponds to ID 1
    circ.cx(0, 1)     # corresponds to ID 2
    circ.cx(0, 2)     # corresponds to ID 3


Then, obtain the simulation path framework qiskit backend. You can choose between the :code:`path_sim_qasm_simulator` and the :code:`path_sim_statevector_simulator`.
The first just yields a dictionary with the counts of the measurements, while the latter also provides the complete statevector (which, depending on the amount of qubits, might not fit in the available memory).

.. code-block:: python3

    from mqt import ddsim

    provider = ddsim.JKQProvider()
    backend = provider.get_backend('path_sim_qasm_simulator')


Per default, the backend uses the :code:`sequential` strategy. For this particular example, this means:
 - first, the Hadamard operation is applied to the initial state (:code:`[0, 1] -> 4`)
 - then, the first CNOT is applied (:code:`[4, 2] -> 5`)
 - finally, the last CNOT is applied (:code:`[5, 3] -> 6`)

 The corresponding simulation path is thus described by :code:`[[0, 1], [4, 2], [5, 3]]` and the final state is the one with ID :code:`6`.

The simulation is started by calling the :code:`execute` function, which takes several optional configuration parameters (such as the simulation path strategy).
For a complete list of configuration options see [here](#configuration).

.. code-block:: python3

    job = execute(circ, backend)
    result = job.result()

    counts = result.get_counts(circ)
    print(counts)


CoTenGra
########

Instead of re-inventing the wheel, the framework allows to translate strategies from the domain of tensor networks to decision diagrams.
To this end, the tensor network contraction library `CoTenGra <https://github.com/jcmgray/cotengra>`_ is used.
In order to use this part of the framework, some extra dependencies have to be installed. This can be accomplished by running

.. code-block:: console

    pip install jkq.ddsim[tnflow]
    pip install -U git+https://github.com/jcmgray/cotengra.git

Then, in order to let CoTenGra determine a simulation path for a given circuit the :code:`mode="cotengra"` option has to be used when calling :code:`execute`, i.e.,

.. code-block:: python3

    job = execute(circ, backend, mode="cotengra")

Per default this uses a maximum of :code:`60s` (option :code:`cotengra_max_time`) and :code:`1024` trials (option :code:`cotengra_max_repeats`) for CoTenGra and dumps a representation of the determined simulation path to a file in the current working directory (option `cotengra_dump_path`).
Optionally, a visualization of the simulation path can be generated by specifying :code:`cotengra_plot_ring=True`.

Configuration
#############

The framework can be configured using multiple options (which can be passed to the :code:`execute` function):
 - :code:`mode`: the simulation path mode to use (:code:`sequential`, :code:`pairwise_recursive`, :code:`bracket`, :code:`alternating`, :code:`cotengra`))
 - :code:`bracket_size`: the bracket size used for the :code:`bracket` mode (default: *`2`*)
 - :code:`alternating_start`: the id of the operation to start with in the :code:`alternating` mode (default: :code:`0`)
 - :code:`seed`: the random seed used for the simulator (default :code:`0`, i.e., no particular seed)

In addition to the above, CoTenGra can be configured using the options described [above](#cotengra).