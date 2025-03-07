import unittest
from qiskit import QuantumCircuit, QuantumRegister
from qiskit import execute

from mqt.ddsim.hybridstatevectorsimulator import HybridStatevectorSimulator


class JKQHybridStatevectorSimulatorTest(unittest.TestCase):
    """Runs backend checks and some very basic functionality tests"""

    def setUp(self):
        self.backend = HybridStatevectorSimulator()
        qr = QuantumRegister(2)
        self.q_circuit = QuantumCircuit(qr)
        self.q_circuit.h(qr[0])
        self.q_circuit.cx(qr[0], qr[1])

    def test_configuration(self):
        """Test backend.configuration()."""
        configuration = self.backend.configuration()
        return configuration

    def test_properties(self):
        """Test backend.properties()."""
        properties = self.backend.properties()
        self.assertEqual(properties, None)

    def test_status(self):
        """Test backend.status()."""
        status = self.backend.status()
        return status

    def test_statevector_output(self):
        """Test final state vector for single circuit run."""
        result = execute(self.q_circuit, backend=self.backend, shots=0).result()
        self.assertEqual(result.success, True)
        actual = result.get_statevector(self.q_circuit)

        self.assertEqual(len(actual), 4)

        # state is 1/sqrt(2)|00> + 1/sqrt(2)|11>, up to a global phase
        self.assertAlmostEqual((abs(actual[0])) ** 2, 1 / 2, places=5)
        self.assertEqual(actual[1], 0)
        self.assertEqual(actual[2], 0)
        self.assertAlmostEqual((abs(actual[3])) ** 2, 1 / 2, places=5)
