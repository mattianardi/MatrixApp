
export enum ProductStatus {
  SAFE = 'safe',
  EXPIRING = 'in_scadenza',
  EXPIRED = 'scaduto'
}

export enum Origin {
  MANUAL = 'manuale',
  SCAN = 'scansione',
  AI = 'AI',
  RECEIPT = 'scontrino'
}

export type Shop = {
  id: string;
  name: string;
  address?: string;
  notes?: string;
};

export type Product = {
  id: string;
  name: string;
  brand?: string;
  category: string;
  location: string;
  quantity: number;
  unit: string;
  expiryDate: string;
  status: ProductStatus;
  origin: Origin;
  shop?: string;
  averagePrice?: number;
  notes?: string;
};

export type ShoppingItem = {
  id: string;
  name: string;
  brand?: string;
  quantity: number;
  unit: string;
  shopId: string;
  isPurchased: boolean;
  confidence?: 'Alta' | 'Media' | 'Bassa';
  reason?: string;
  notes?: string;
};

export type CartHistory = {
  id: string;
  shopId: string;
  date: string;
  totalSpent: number;
  items: {
    name: string;
    quantity: number;
    price: number;
    category: string;
  }[];
  receiptImage?: string;
};

export type Recipe = {
  title: string;
  description: string;
  ingredients: string[];
  missingIngredients: string[];
  difficulty: string;
  time: number;
  instructions?: string;
  youtubeId?: string;
  url?: string;
};

export type AppState = {
  products: Product[];
  shoppingList: ShoppingItem[];
  shops: Shop[];
  history: CartHistory[];
}
