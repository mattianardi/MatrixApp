
import React from 'react';
import { Product, ProductStatus } from '../types';

interface ProductCardProps {
  product: Product;
  onCook: (p: Product) => void;
  onUpdateQuantity: (id: string, newQty: number) => void;
  onDelete: (id: string) => void;
  onEdit: (p: Product) => void;
}

const ProductCard: React.FC<ProductCardProps> = ({ product, onCook, onUpdateQuantity, onDelete, onEdit }) => {
  const getStatusConfig = (status: ProductStatus) => {
    switch (status) {
      case ProductStatus.EXPIRED: 
        return { accent: 'bg-rose-500', badgeBg: 'bg-rose-500/10', badgeText: 'text-rose-400', label: 'Scaduto' };
      case ProductStatus.EXPIRING: 
        return { accent: 'bg-amber-500', badgeBg: 'bg-amber-500/10', badgeText: 'text-amber-400', label: 'In scadenza' };
      default: 
        return { accent: 'bg-emerald-500', badgeBg: 'bg-emerald-500/10', badgeText: 'text-emerald-400', label: 'Fresco' };
    }
  };

  const config = getStatusConfig(product.status);

  const getCategoryIcon = (cat: string) => {
    switch (cat) {
      case 'Latticini': return '🥛';
      case 'Carne/Pesce': return '🥩';
      case 'Verdura': return '🥦';
      case 'Frutta': return '🍎';
      case 'Bevande': return '🥤';
      case 'Cucinato': return '🍳';
      default: return '📦';
    }
  };

  const formatDate = (dateStr: string) => {
    const d = new Date(dateStr);
    return d.toLocaleDateString('it-IT', { day: 'numeric', month: 'short' });
  };

  return (
    <div className="relative bg-[#151821] rounded-xl overflow-hidden shadow-md border border-white/5 transition-all active:scale-[0.99]">
      <div className={`absolute left-0 top-0 bottom-0 w-[4px] ${config.accent}`} />
      
      <div className="p-3 pl-4">
        <div className="flex items-start justify-between gap-2">
          <div className="flex items-center gap-3">
            <div className="w-10 h-10 bg-white/5 rounded-xl flex items-center justify-center text-xl">
              {getCategoryIcon(product.category)}
            </div>
            
            <div className="flex flex-col">
              <h3 className="text-sm font-bold text-white leading-tight line-clamp-1">{product.name}</h3>
              <div className="flex items-center gap-2 mt-0.5">
                <span className="text-[9px] font-bold uppercase tracking-wider bg-white/5 border border-white/10 px-1.5 py-0.2 rounded text-gray-400">
                  {product.category}
                </span>
                <div className="flex items-center gap-1 text-[10px] text-gray-400">
                  <svg className="w-3 h-3" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6" /></svg>
                  {product.location}
                </div>
              </div>
            </div>
          </div>

          <div className="flex flex-col items-end gap-1.5">
            <div className={`px-2 py-0.5 rounded text-[8px] font-black uppercase tracking-wider ${config.badgeBg} ${config.badgeText}`}>
              {config.label}
            </div>
            <div className="flex items-center gap-0.5">
              <button onClick={() => onEdit(product)} className="p-1 text-gray-600 hover:text-white transition-colors">
                <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z" /></svg>
              </button>
              <button onClick={() => onDelete(product.id)} className="p-1 text-gray-600 hover:text-rose-400 transition-colors">
                <svg className="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" /></svg>
              </button>
            </div>
          </div>
        </div>

        <div className="mt-3 flex items-center justify-between border-t border-white/5 pt-2">
          <div className="flex items-center gap-2">
             <button 
                onClick={() => onCook(product)}
                className="flex items-center gap-1 px-2 py-1 bg-emerald-500/10 text-emerald-400 rounded-lg text-[9px] font-black uppercase tracking-widest hover:bg-emerald-500 hover:text-white transition-all"
             >
               🍳 Cucina
             </button>
             <p className="text-[10px] font-semibold text-gray-500">
               Qta: <span className="text-white">{product.quantity}{product.unit}</span>
             </p>
             <p className="text-[10px] text-gray-500">
               Scade: <span className="text-gray-300">{formatDate(product.expiryDate)}</span>
             </p>
          </div>
          <div className="flex items-center gap-1.5">
             <button onClick={() => onUpdateQuantity(product.id, Math.max(0, product.quantity - 1))} className="w-7 h-7 bg-white/5 rounded-lg flex items-center justify-center text-gray-400 hover:bg-white/10 text-sm font-bold border border-white/5">-</button>
             <button onClick={() => onUpdateQuantity(product.id, product.quantity + 1)} className="w-7 h-7 bg-white/5 rounded-lg flex items-center justify-center text-gray-400 hover:bg-white/10 text-sm font-bold border border-white/5">+</button>
          </div>
        </div>
      </div>
    </div>
  );
};

export default ProductCard;
