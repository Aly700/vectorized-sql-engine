SELECT big.k, mid.k, tiny.k FROM big JOIN mid ON big.k = mid.k JOIN tiny ON mid.k = tiny.k
