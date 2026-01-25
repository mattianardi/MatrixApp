
import { Product, ProductStatus, Origin, ShoppingItem, Shop } from './types';

export const CATEGORIES = [
  'Latticini', 'Carne/Pesce', 'Verdura', 'Frutta', 'Secchi', 'Bevande', 'Cucinato', 'Altro'
];

export const LOCATIONS = [
  'Frigo', 'Freezer', 'Dispensa', 'Cantina'
];

export const MOCK_SHOPS: Shop[] = [
  { id: 'sh1', name: 'Lidl', address: 'Via Roma 10, Milano', notes: 'Conveniente per latticini' },
  { id: 'sh2', name: 'Conad', address: 'Piazza Duomo 1, Milano', notes: 'Ottima carne' },
  { id: 'sh3', name: 'Amazon', notes: 'Per prodotti a lunga scadenza' },
  { id: 'sh4', name: 'Farmacia', address: 'Viale Monza 5, Milano' }
];

export const MOCK_PRODUCTS: Product[] = [
  {
    id: '1',
    name: 'Mozzarelle',
    brand: 'Santa Lucia',
    category: 'Latticini',
    location: 'Frigo',
    quantity: 4,
    unit: 'pz',
    expiryDate: '2025-05-24',
    status: ProductStatus.SAFE,
    origin: Origin.SCAN,
    shop: 'Lidl',
    averagePrice: 2.50
  },
  {
    id: '2',
    name: 'Uova Bio',
    category: 'Latticini',
    location: 'Dispensa',
    quantity: 12,
    unit: 'pz',
    expiryDate: '2025-05-15',
    status: ProductStatus.EXPIRING,
    origin: Origin.MANUAL,
    averagePrice: 3.20
  },
  {
    id: '3',
    name: 'Pollo ruspante',
    category: 'Carne/Pesce',
    location: 'Frigo',
    quantity: 500,
    unit: 'g',
    expiryDate: '2025-06-10',
    status: ProductStatus.SAFE,
    origin: Origin.RECEIPT,
    shop: 'Conad'
  },
  {
    id: '4',
    name: 'Latte Intero',
    category: 'Bevande',
    location: 'Frigo',
    quantity: 1,
    unit: 'l',
    expiryDate: '2024-12-01',
    status: ProductStatus.EXPIRED,
    origin: Origin.MANUAL,
    averagePrice: 1.20
  }
];

export const MOCK_SHOPPING: ShoppingItem[] = [
  {
    id: 's1',
    name: 'Yogurt Muller',
    quantity: 2,
    unit: 'pz',
    shopId: 'sh1',
    isPurchased: false,
    confidence: 'Alta'
  }
];
